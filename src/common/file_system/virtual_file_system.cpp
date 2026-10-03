#include "common/file_system/virtual_file_system.h"

#include <cctype>

#include "common/assert.h"
#include "common/exception/io.h"
#include "common/file_system/gzip_file_system.h"
#include "common/file_system/local_file_system.h"
#include "common/string_utils.h"
#include "main/client_context.h"
#include "main/database.h"

namespace lbug {
namespace common {

static bool isRelativePath(const std::string& path) {
    if (path.empty() || path[0] == '/' || path[0] == '~') {
        return false;
    }
    if (path.find("://") != std::string::npos) {
        return false;
    }
    return !(
        path.size() > 1 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':');
}

static std::string joinRemotePath(const std::string& base, std::string path) {
    while (path.starts_with("./")) {
        path = path.substr(2);
    }
    if (path == ".") {
        path.clear();
    }
    if (path.empty() || base.ends_with('/')) {
        return base + path;
    }
    return base + "/" + path;
}

VirtualFileSystem::VirtualFileSystem() : VirtualFileSystem{""} {}

VirtualFileSystem::VirtualFileSystem(std::string homeDir) {
    defaultFS = std::make_unique<LocalFileSystem>(homeDir);
    compressedFileSystem.emplace(FileCompressionType::GZIP, std::make_unique<GZipFileSystem>());
}

VirtualFileSystem::~VirtualFileSystem() = default;

void VirtualFileSystem::registerFileSystem(std::unique_ptr<FileSystem> fileSystem) {
    subSystems.push_back(std::move(fileSystem));
}

FileCompressionType VirtualFileSystem::autoDetectCompressionType(const std::string& path) {
    if (isGZIPCompressed(path)) {
        return FileCompressionType::GZIP;
    }
    return FileCompressionType::UNCOMPRESSED;
}

std::unique_ptr<FileInfo> VirtualFileSystem::openFile(const std::string& path, FileOpenFlags flags,
    main::ClientContext* context) {
    auto compressionType = flags.compressionType;
    if (compressionType == FileCompressionType::AUTO_DETECT) {
        compressionType = autoDetectCompressionType(path);
    }
    auto fileHandle = findFileSystem(path)->openFile(path, flags, context);
    if (compressionType == FileCompressionType::UNCOMPRESSED) {
        return fileHandle;
    }
    if (flags.flags & FileFlags::WRITE) {
        throw IOException{"Writing to compressed files is not supported yet."};
    }
    if (StringUtils::getLower(getFileExtension(path)) != ".csv") {
        throw IOException{"Lbug currently only supports reading from compressed csv files."};
    }
    return compressedFileSystem.at(compressionType)->openCompressedFile(std::move(fileHandle));
}

std::vector<std::string> VirtualFileSystem::glob(main::ClientContext* context,
    const std::string& path) const {
    return findFileSystem(path)->glob(context, path);
}

void VirtualFileSystem::overwriteFile(const std::string& from, const std::string& to) {
    findFileSystem(from)->overwriteFile(from, to);
}

void VirtualFileSystem::renameFile(const std::string& from, const std::string& to) {
    auto* sourceFileSystem = findFileSystem(from);
    if (sourceFileSystem != findFileSystem(to)) {
        throw IOException{"Cannot rename files across file systems."};
    }
    sourceFileSystem->renameFile(from, to);
}

void VirtualFileSystem::createDir(const std::string& dir) const {
    findFileSystem(dir)->createDir(dir);
}

void VirtualFileSystem::removeFileIfExists(const std::string& path,
    const main::ClientContext* context) {
    findFileSystem(path)->removeFileIfExists(path, context);
}

bool VirtualFileSystem::fileOrPathExists(const std::string& path, main::ClientContext* context) {
    return findFileSystem(path)->fileOrPathExists(path, context);
}

std::string VirtualFileSystem::expandPath(main::ClientContext* context,
    const std::string& path) const {
    return findFileSystem(path)->expandPath(context, path);
}

void VirtualFileSystem::readFromFile(FileInfo& /*fileInfo*/, void* /*buffer*/,
    uint64_t /*numBytes*/, uint64_t /*position*/) const {
    UNREACHABLE_CODE;
}

int64_t VirtualFileSystem::readFile(FileInfo& /*fileInfo*/, void* /*buf*/, size_t /*nbyte*/) const {
    UNREACHABLE_CODE;
}

void VirtualFileSystem::writeFile(FileInfo& /*fileInfo*/, const uint8_t* /*buffer*/,
    uint64_t /*numBytes*/, uint64_t /*offset*/) const {
    UNREACHABLE_CODE;
}

void VirtualFileSystem::syncFile(const FileInfo& fileInfo) const {
    findFileSystem(fileInfo.path)->syncFile(fileInfo);
}

void VirtualFileSystem::syncParentDirectory(const std::string& path) const {
    auto* fileSystem = findFileSystem(path);
    if (fileSystem == defaultFS.get()) {
        LocalFileSystem::syncParentDirectory(path);
    } else if (auto* directorySyncFileSystem =
                   dynamic_cast<const DirectorySyncFileSystem*>(fileSystem)) {
        directorySyncFileSystem->syncParentDirectory(path);
    }
}

void VirtualFileSystem::cleanUP(main::ClientContext* context) {
    for (auto& subSystem : subSystems) {
        subSystem->cleanUP(context);
    }
    defaultFS->cleanUP(context);
}

bool VirtualFileSystem::handleFileViaFunction(const std::string& path) const {
    return findFileSystem(path)->handleFileViaFunction(path);
}

function::TableFunction VirtualFileSystem::getHandleFunction(const std::string& path) const {
    return findFileSystem(path)->getHandleFunction(path);
}

int64_t VirtualFileSystem::seek(FileInfo& /*fileInfo*/, uint64_t /*offset*/, int /*whence*/) const {
    UNREACHABLE_CODE;
}

void VirtualFileSystem::truncate(FileInfo& /*fileInfo*/, uint64_t /*size*/) const {
    UNREACHABLE_CODE;
}

uint64_t VirtualFileSystem::getFileSize(const FileInfo& /*fileInfo*/) const {
    UNREACHABLE_CODE;
}

FileSystem* VirtualFileSystem::findFileSystem(const std::string& path) const {
    for (auto& subSystem : subSystems) {
        if (subSystem->canHandleFile(path)) {
            return subSystem.get();
        }
    }
    return defaultFS.get();
}

VirtualFileSystem* VirtualFileSystem::GetUnsafe(const main::ClientContext& context) {
    return context.getDatabase()->getVFS();
}

std::string VirtualFileSystem::resolvePath(main::ClientContext* context, const std::string& path) {
    if (!context) {
        return path;
    }
    auto vfs = GetUnsafe(*context);
    if (!vfs) {
        return path;
    }
    auto paths = vfs->glob(context, path);
    if (!paths.empty()) {
        return paths.front();
    }
    if (!isRelativePath(path)) {
        return path;
    }
    const auto& fileSearchPath = context->getClientConfig()->fileSearchPath;
    if (fileSearchPath.empty()) {
        return path;
    }
    for (auto& searchPath : StringUtils::split(fileSearchPath, ",")) {
        if (searchPath.find("://") == std::string::npos) {
            continue;
        }
        paths = vfs->glob(context, joinRemotePath(searchPath, path));
        if (!paths.empty()) {
            return paths.front();
        }
    }
    return path;
}

} // namespace common
} // namespace lbug

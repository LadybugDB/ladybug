#include "main/database.h"

#include "extension/binder_extension.h"
#include "extension/extension_manager.h"
#include "extension/mapper_extension.h"
#include "extension/planner_extension.h"
#include "extension/transformer_extension.h"
#include "main/client_context.h"
#include "main/database_manager.h"
#include "storage/buffer_manager/buffer_manager.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include "common/exception/exception.h"
#include "common/file_system/virtual_file_system.h"
#include "main/db_config.h"
#include "processor/processor.h"
#include "storage/storage_extension.h"
#include "storage/storage_manager.h"
#include "storage/storage_utils.h"
#include "transaction/transaction_manager.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace main {
namespace {

// Returns the cgroup memory limit in bytes, or UINT64_MAX if no limit is set or
// if the limit cannot be determined (non-Linux, unreadable files, ...).
// Inside a container the host's physical memory (sysconf(_SC_PHYS_PAGES)) is
// visible, so the default buffer pool must be sized from the container's limit
// instead (see #1070).
uint64_t getCGroupMemoryLimit() {
#if defined(__linux__)
    // cgroup v2, then cgroup v1 locations.
    const char* paths[] = {
        "/sys/fs/cgroup/memory.max",
        "/sys/fs/cgroup/memory/memory.limit_in_bytes",
        "/sys/fs/cgroup/memory.limit_in_bytes",
    };
    for (const char* path : paths) {
        std::ifstream file(path);
        if (!file.is_open()) {
            continue;
        }
        std::string content;
        std::getline(file, content);
        // Trim trailing whitespace (e.g. the trailing newline).
        while (!content.empty() && (content.back() == '\n' || content.back() == '\r' ||
                                       content.back() == ' ' || content.back() == '\t')) {
            content.pop_back();
        }
        if (content.empty() || content == "max") {
            // "max" means no limit on cgroup v2.
            continue;
        }
        try {
            auto limit = std::stoull(content);
            if (limit == 0) {
                continue;
            }
            return limit;
        } catch (...) {
            continue;
        }
    }
#endif
    return UINT64_MAX;
}

uint64_t getAvailableSystemMemory() {
#if defined(_WIN32)
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    GlobalMemoryStatusEx(&status);
    return static_cast<uint64_t>(status.ullTotalPhys);
#else
    auto hostMemSize = static_cast<uint64_t>(sysconf(_SC_PHYS_PAGES)) *
                       static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    return std::min(hostMemSize, getCGroupMemoryLimit());
#endif
}

} // namespace

namespace {
// In-process guard for nodejs#29: POSIX fcntl locks are per-process, so a second
// Database on the same path in the same process would otherwise open successfully
// and its WAL would be stolen by the first DB's close-time checkpoint.
// Track file-backed read-write opens; read-only and in-memory opens are exempt
// (read-only second open while RW is alive is relied on by
// WalTest.LargeWALRecordReplayWithPreviousDBAlive).
//
// Claims are keyed by the path with symlinks resolved and, once the file exists, by its
// (device, inode), so a hard link or another spelling of the same file is refused too.
using FileIdentity = std::pair<uint64_t, uint64_t>;
std::mutex openDbMtx;
std::unordered_map<std::string, std::optional<FileIdentity>> openRwDbPaths;
std::set<FileIdentity> openRwDbFiles;

std::string claimKey(const std::string& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    return ec ? path : canonical.string();
}

std::optional<FileIdentity> fileIdentity(const std::string& path) {
#ifdef _WIN32
    // Windows file locks belong to the handle and already refuse a second writable open of the
    // same file, whatever path it is reached by.
    (void)path;
    return std::nullopt;
#else
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        return std::nullopt;
    }
    return FileIdentity{static_cast<uint64_t>(st.st_dev), static_cast<uint64_t>(st.st_ino)};
#endif
}

bool tryClaimOpenDbPath(const std::string& path) {
    const auto key = claimKey(path);
    const auto identity = fileIdentity(path);
    std::lock_guard<std::mutex> lck{openDbMtx};
    if (openRwDbPaths.contains(key) || (identity && openRwDbFiles.contains(*identity))) {
        return false;
    }
    openRwDbPaths.emplace(key, identity);
    if (identity) {
        openRwDbFiles.insert(*identity);
    }
    return true;
}

// A database file created by this open had no identity when it was claimed. Record it now, so
// a hard link made to it later is refused as well.
void recordOpenDbFileIdentity(const std::string& path) {
    const auto identity = fileIdentity(path);
    if (!identity) {
        return;
    }
    std::lock_guard<std::mutex> lck{openDbMtx};
    auto it = openRwDbPaths.find(claimKey(path));
    if (it != openRwDbPaths.end() && !it->second) {
        it->second = identity;
        openRwDbFiles.insert(*identity);
    }
}

void releaseOpenDbPath(const std::string& path) {
    std::lock_guard<std::mutex> lck{openDbMtx};
    auto it = openRwDbPaths.find(claimKey(path));
    if (it == openRwDbPaths.end()) {
        return;
    }
    if (it->second) {
        openRwDbFiles.erase(*it->second);
    }
    openRwDbPaths.erase(it);
}
} // namespace

SystemConfig::SystemConfig(uint64_t bufferPoolSize_, uint64_t maxNumThreads, bool enableCompression,
    bool readOnly, uint64_t maxDBSize, bool autoCheckpoint, uint64_t checkpointThreshold,
    bool forceCheckpointOnClose, bool throwOnWalReplayFailure, bool enableChecksums,
    bool enableMultiWrites, bool enableDefaultHashIndex, uint64_t physicalPlanPoolPerThread
#if defined(__APPLE__)
    ,
    uint32_t threadQos
#endif
    )
    : maxNumThreads{maxNumThreads}, enableCompression{enableCompression}, readOnly{readOnly},
      autoCheckpoint{autoCheckpoint}, checkpointThreshold{checkpointThreshold},
      forceCheckpointOnClose{forceCheckpointOnClose},
      throwOnWalReplayFailure(throwOnWalReplayFailure), enableChecksums(enableChecksums),
      enableMultiWrites{enableMultiWrites}, enableDefaultHashIndex{enableDefaultHashIndex},
      physicalPlanPoolPerThread{physicalPlanPoolPerThread} {
#if defined(__APPLE__)
    this->threadQos = threadQos;
#endif
    if (bufferPoolSize_ == -1u || bufferPoolSize_ == 0) {
        auto systemMemSize = getAvailableSystemMemory();
        bufferPoolSize_ = static_cast<uint64_t>(
            BufferPoolConstants::DEFAULT_PHY_MEM_SIZE_RATIO_FOR_BM *
            static_cast<double>(std::min(systemMemSize, static_cast<uint64_t>(UINTPTR_MAX))));
        // On 32-bit systems or systems with extremely large memory, the buffer pool size may
        // exceed the maximum size of a VMRegion. In this case, we set the buffer pool size to
        // 80% of the maximum size of a VMRegion.
        bufferPoolSize_ = static_cast<uint64_t>(std::min(static_cast<double>(bufferPoolSize_),
            BufferPoolConstants::DEFAULT_VM_REGION_MAX_SIZE *
                BufferPoolConstants::DEFAULT_PHY_MEM_SIZE_RATIO_FOR_BM));
    }
    bufferPoolSize = bufferPoolSize_;
#ifndef __SINGLE_THREADED__
    if (maxNumThreads == 0) {
        this->maxNumThreads = std::thread::hardware_concurrency();
    }
#else
    // In single-threaded mode, even if the user specifies a number of threads,
    // it will be ignored and set to 0.
    this->maxNumThreads = 1;
#endif
    if (maxDBSize == -1u) {
        maxDBSize = BufferPoolConstants::DEFAULT_VM_REGION_MAX_SIZE;
    }
    this->maxDBSize = maxDBSize;
}

Database::Database(std::string_view databasePath, SystemConfig systemConfig)
    : Database(databasePath, systemConfig, initBufferManager) {}

Database::Database(std::string_view databasePath, SystemConfig systemConfig,
    construct_bm_func_t constructBMFunc)
    : dbConfig{std::make_unique<DBConfig>(systemConfig)} {
    initMembers(databasePath, constructBMFunc);
}

std::unique_ptr<BufferManager> Database::initBufferManager(const Database& db) {
    return std::make_unique<BufferManager>(db.databasePath,
        StorageUtils::getTmpFilePath(db.databasePath), db.dbConfig->bufferPoolSize,
        db.dbConfig->maxDBSize, db.vfs.get(), db.dbConfig->readOnly);
}

void Database::initMembers(std::string_view dbPath, construct_bm_func_t initBmFunc) {
    // To expand a path with home directory(~), we have to pass in a dummy clientContext which
    // handles the home directory expansion.
    const auto dbPathStr = std::string(dbPath);
    auto clientContext = ClientContext(this);
    databasePath = StorageUtils::expandPath(&clientContext, dbPathStr);
    clientContext.addDBDirToFileSearchPath(databasePath);

    if (std::filesystem::is_directory(databasePath)) {
        throw RuntimeException("Database path cannot be a directory: " + databasePath);
    }
    vfs = std::make_unique<VirtualFileSystem>(databasePath);
    validatePathInReadOnly();
    // Claim the path before any WAL/data file is touched so a second RW open
    // in this process fails fast instead of corrupting the first DB's WAL on close.
    const bool needsOpenClaim = !DBConfig::isDBPathInMemory(databasePath) && !dbConfig->readOnly;
    if (needsOpenClaim) {
        if (!tryClaimOpenDbPath(databasePath)) {
            throw RuntimeException("Cannot open database at " + databasePath +
                                   " in read-write mode: it is already open in this process. " +
                                   "Close the existing Database first.");
        }
        ownsOpenPathClaim = true;
    }
    try {
        bufferManager = initBmFunc(*this);
        memoryManager = std::make_unique<MemoryManager>(bufferManager.get(), vfs.get());
#if defined(__APPLE__)
        queryProcessor = std::make_unique<processor::QueryProcessor>(dbConfig->maxNumThreads,
            dbConfig->threadQos);
#else
        queryProcessor = std::make_unique<processor::QueryProcessor>(dbConfig->maxNumThreads);
#endif

        catalog = std::make_unique<Catalog>();
        storageManager = std::make_unique<StorageManager>(databasePath, dbConfig->readOnly,
            dbConfig->enableChecksums, *memoryManager, dbConfig->enableCompression,
            dbConfig->enableDefaultHashIndex, vfs.get());
        transactionManager = std::make_unique<TransactionManager>(storageManager->getWAL());
        databaseManager = std::make_unique<DatabaseManager>();

        extensionManager = std::make_unique<extension::ExtensionManager>();
        dbLifeCycleManager = std::make_shared<DatabaseLifeCycleManager>();
        if (clientContext.isInMemory()) {
            storageManager->initDataFileHandle(vfs.get(), &clientContext);
            extensionManager->autoLoadLinkedExtensions(&clientContext);
            return;
        }
        StorageManager::recover(clientContext, dbConfig->throwOnWalReplayFailure,
            dbConfig->enableChecksums);

        // Load graphs from system catalog
        databaseManager->loadGraphsFromCatalog(memoryManager.get(), &clientContext);
        if (ownsOpenPathClaim) {
            recordOpenDbFileIdentity(databasePath);
        }
    } catch (...) {
        if (ownsOpenPathClaim) {
            // Close this open's files before another open can claim the path (see
            // releaseMembersBeforeOpenPathClaim).
            releaseMembersBeforeOpenPathClaim();
            releaseOpenDbPath(databasePath);
            ownsOpenPathClaim = false;
        }
        throw;
    }
}

void Database::releaseMembersBeforeOpenPathClaim() {
    // Closing any descriptor of a file drops all of this process's POSIX locks on it. If the
    // claim were released first, a writable open on another thread could take its lock in
    // between, and closing this open's descriptors would then drop that lock. So the members
    // that own files are destroyed here, in the same order as implicit member destruction
    // (reverse declaration order), before the claim is released.
    mapperExtensions.clear();
    plannerExtensions.clear();
    binderExtensions.clear();
    transformerExtensions.clear();
    dbLifeCycleManager.reset();
    extensionManager.reset();
    databaseManager.reset();
    lockFile.reset();
    transactionManager.reset();
    storageManager.reset();
    catalog.reset();
    queryProcessor.reset();
    memoryManager.reset();
    bufferManager.reset();
}

Database::~Database() {
    if (!dbConfig->readOnly && dbConfig->forceCheckpointOnClose) {
        try {
            ClientContext clientContext(this);
            transactionManager->checkpoint(clientContext);
        } catch (...) {} // NOLINT
    }
    dbLifeCycleManager->isDatabaseClosed = true;
    if (ownsOpenPathClaim) {
        releaseMembersBeforeOpenPathClaim();
        releaseOpenDbPath(databasePath);
        ownsOpenPathClaim = false;
    }
}

// NOLINTNEXTLINE(readability-make-member-function-const): Semantically non-const function.
void Database::registerFileSystem(std::unique_ptr<FileSystem> fs) {
    vfs->registerFileSystem(std::move(fs));
}

// NOLINTNEXTLINE(readability-make-member-function-const): Semantically non-const function.
void Database::registerStorageExtension(std::string name,
    std::unique_ptr<StorageExtension> storageExtension) {
    extensionManager->registerStorageExtension(std::move(name), std::move(storageExtension));
}

// NOLINTNEXTLINE(readability-make-member-function-const): Semantically non-const function.
void Database::addExtensionOption(std::string name, LogicalTypeID type, Value defaultValue,
    bool isConfidential) {
    extensionManager->addExtensionOption(std::move(name), type, std::move(defaultValue),
        isConfidential);
}

void Database::addTransformerExtension(
    std::unique_ptr<extension::TransformerExtension> transformerExtension) {
    transformerExtensions.push_back(std::move(transformerExtension));
}

std::vector<extension::TransformerExtension*> Database::getTransformerExtensions() {
    std::vector<extension::TransformerExtension*> transformers;
    for (auto& transformerExtension : transformerExtensions) {
        transformers.push_back(transformerExtension.get());
    }
    return transformers;
}

void Database::addBinderExtension(
    std::unique_ptr<extension::BinderExtension> transformerExtension) {
    binderExtensions.push_back(std::move(transformerExtension));
}

std::vector<extension::BinderExtension*> Database::getBinderExtensions() {
    std::vector<extension::BinderExtension*> binders;
    for (auto& binderExtension : binderExtensions) {
        binders.push_back(binderExtension.get());
    }
    return binders;
}

void Database::addPlannerExtension(std::unique_ptr<extension::PlannerExtension> plannerExtension) {
    plannerExtensions.push_back(std::move(plannerExtension));
}

std::vector<extension::PlannerExtension*> Database::getPlannerExtensions() {
    std::vector<extension::PlannerExtension*> planners;
    for (auto& plannerExtension : plannerExtensions) {
        planners.push_back(plannerExtension.get());
    }
    return planners;
}

void Database::addMapperExtension(std::unique_ptr<extension::MapperExtension> mapperExtension) {
    mapperExtensions.push_back(std::move(mapperExtension));
}

std::vector<extension::MapperExtension*> Database::getMapperExtensions() {
    std::vector<extension::MapperExtension*> mappers;
    for (auto& mapperExtension : mapperExtensions) {
        mappers.push_back(mapperExtension.get());
    }
    return mappers;
}

std::vector<StorageExtension*> Database::getStorageExtensions() {
    return extensionManager->getStorageExtensions();
}

bool Database::isReadOnly() const {
    return dbConfig->readOnly;
}

bool Database::isMultiWritesEnabled() const {
    return dbConfig->enableMultiWrites;
}

void Database::validatePathInReadOnly() const {
    if (dbConfig->readOnly) {
        if (DBConfig::isDBPathInMemory(databasePath)) {
            throw Exception("Cannot open an in-memory database under READ ONLY mode.");
        }
        if (!vfs->fileOrPathExists(databasePath)) {
            throw Exception("Cannot create an empty database under READ ONLY mode.");
        }
    }
}

uint64_t Database::getNextQueryID() {
    std::unique_lock lock(queryIDGenerator.queryIDLock);
    return queryIDGenerator.queryID++;
}

} // namespace main
} // namespace lbug

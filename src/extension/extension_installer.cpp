#include "extension/extension_installer.h"

#include <atomic>
#include <thread>

#include "common/exception/io.h"
#include "common/file_system/virtual_file_system.h"
#include "httplib.h"
#include "main/client_context.h"
#include <format>

namespace lbug {
namespace extension {

namespace {

void applyProxyConfig(httplib::Client& client, const ExtensionRepoInfo& repoInfo) {
    auto proxyConfig = ExtensionUtils::getProxyConfigForURL(repoInfo.hostURL);
    if (!proxyConfig) {
        return;
    }
    client.set_proxy(proxyConfig->host, proxyConfig->port);
    if (!proxyConfig->username.empty()) {
        client.set_proxy_basic_auth(proxyConfig->username, proxyConfig->password);
    }
}

void configureSSLCerts(httplib::Client& client, const ExtensionRepoInfo& repoInfo) {
    // Only relevant for https - plain http/file clients have no SSL context.
    if (repoInfo.hostURL.rfind("https://", 0) != 0) {
        return;
    }
    if (auto caCertPath = ExtensionUtils::getCaCertPath()) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        client.set_ca_cert_path(caCertPath->caCertFilePath, caCertPath->caCertDirPath);
#endif
    }
}

httplib::Client createExtensionDownloadClient(const ExtensionRepoInfo& repoInfo) {
    httplib::Client client(repoInfo.hostURL.c_str());
    configureSSLCerts(client, repoInfo);
    applyProxyConfig(client, repoInfo);
    return client;
}

} // namespace

void ExtensionInstaller::tryDownloadExtensionFile(const ExtensionRepoInfo& repoInfo,
    const std::string& localFilePath) {
    auto cli = createExtensionDownloadClient(repoInfo);
    httplib::Headers headers = {{"User-Agent", std::format("lbug/v{}", LBUG_EXTENSION_VERSION)}};
    auto res = cli.Get(repoInfo.hostPath.c_str(), headers);
    if (!res || res->status != 200) {
        if (res.error() == httplib::Error::Success) {
            // LCOV_EXCL_START
            throw common::IOException(
                std::format("HTTP Returns: {}, Failed to download extension: \"{}\" from {}.",
                    res.value().status, info.name, repoInfo.repoURL));
            // LCOC_EXCL_STOP
        } else {
            throw common::IOException(
                std::format("Failed to download extension: {} at URL {} (ERROR: {})", info.name,
                    repoInfo.repoURL, to_string(res.error())));
        }
    }

    // Download to a temp file, then atomically rename into place so that concurrent
    // readers (e.g. dlopen) never see a truncated or partially-written file.
    auto vfs = common::VirtualFileSystem::GetUnsafe(context);
    static std::atomic<uint64_t> tempCounter{0};
    std::hash<std::thread::id> threadHasher;
    auto tempPath = localFilePath + ".tmp." +
                    std::to_string(threadHasher(std::this_thread::get_id())) + "." +
                    std::to_string(tempCounter.fetch_add(1, std::memory_order_relaxed));

    auto fileInfo = vfs->openFile(tempPath,
        common::FileOpenFlags(common::FileFlags::WRITE | common::FileFlags::READ_ONLY |
                              common::FileFlags::CREATE_AND_TRUNCATE_IF_EXISTS));
    try {
        fileInfo->writeFile(reinterpret_cast<const uint8_t*>(res->body.c_str()), res->body.size(),
            0 /* offset */);
        fileInfo->syncFile();
    } catch (...) {
        // Clean up the temp file on write failure before rethrowing.
        fileInfo.reset();
        vfs->removeFileIfExists(tempPath);
        throw;
    }
    fileInfo.reset(); // Ensure the file handle is closed before rename.
    try {
        vfs->renameFile(tempPath, localFilePath);
    } catch (...) {
        // Clean up the temp file on rename failure (e.g. cross-filesystem edge case).
        vfs->removeFileIfExists(tempPath);
        throw;
    }
}

bool ExtensionInstaller::install() {
    auto install = installExtension();
    if (install) {
        installDependencies();
    }
    return install;
}

bool ExtensionInstaller::installExtension() {
    auto vfs = common::VirtualFileSystem::GetUnsafe(context);
    auto localExtensionDir = context.getExtensionDir();
    if (!vfs->fileOrPathExists(localExtensionDir, &context)) {
        vfs->createDir(localExtensionDir);
    }
    auto localDirForExtension =
        extension::ExtensionUtils::getLocalDirForExtension(&context, info.name);
    if (!vfs->fileOrPathExists(localDirForExtension)) {
        vfs->createDir(localDirForExtension);
    }
    auto localLibFilePath =
        extension::ExtensionUtils::getLocalPathForExtensionLib(&context, info.name);
    if (vfs->fileOrPathExists(localLibFilePath) && !info.forceInstall) {
        // The extension has been installed, skip downloading from the repo.
        return false;
    }
    auto localDirForSharedLib = extension::ExtensionUtils::getLocalPathForSharedLib(&context);
    if (!vfs->fileOrPathExists(localDirForSharedLib)) {
        vfs->createDir(localDirForSharedLib);
    }
    // Re-check after creating dirs: another process may have installed it while we
    // were working.  Without this, both processes would download and rename, wasting
    // bandwidth (though atomic rename prevents data races in dlopen).
    if (vfs->fileOrPathExists(localLibFilePath) && !info.forceInstall) {
        return false;
    }
    auto libFileRepoInfo = extension::ExtensionUtils::getExtensionLibRepoInfo(info.name, info.repo);

    tryDownloadExtensionFile(libFileRepoInfo, localLibFilePath);
    return true;
}

void ExtensionInstaller::installDependencies() {
    auto extensionRepoInfo = ExtensionUtils::getExtensionInstallerRepoInfo(info.name, info.repo);
    auto cli = createExtensionDownloadClient(extensionRepoInfo);
    httplib::Headers headers = {{"User-Agent", std::format("lbug/v{}", LBUG_EXTENSION_VERSION)}};
    auto res = cli.Get(extensionRepoInfo.hostPath.c_str(), headers);
    if (!res || res->status != 200) {
        // The extension doesn't have an installer.
        return;
    }
    auto extensionInstallerPath =
        ExtensionUtils::getLocalPathForExtensionInstaller(&context, info.name);
    tryDownloadExtensionFile(extensionRepoInfo, extensionInstallerPath);
    auto libLoader = ExtensionLibLoader(info.name, extensionInstallerPath.c_str());
    auto install = libLoader.getInstallFunc();
    (*install)(info.repo, context);
}

} // namespace extension
} // namespace lbug

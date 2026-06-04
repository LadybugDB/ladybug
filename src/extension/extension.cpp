#include "extension/extension.h"

#include <cstdlib>

#include "common/exception/io.h"
#include "common/string_utils.h"
#include "common/system_message.h"
#include "main/client_context.h"
#include "main/database.h"
#include "storage/storage_manager.h"
#ifdef _WIN32

#include "windows.h"
#define RTLD_NOW 0
#define RTLD_LOCAL 0

#else
#include <dlfcn.h>

#include <format>
#endif

namespace lbug {
namespace extension {

namespace {

struct ParsedURL {
    std::string scheme;
    std::string host;
    int port = -1;
};

std::string getEnv(const char* name) {
    const auto value = std::getenv(name); // NOLINT(*-mt-unsafe)
    return value == nullptr ? "" : value;
}

std::string getProxyEnv(std::initializer_list<const char*> names) {
    for (auto name : names) {
        auto value = getEnv(name);
        if (!value.empty()) {
            return value;
        }
    }
    return "";
}

int parsePort(const std::string& port) {
    try {
        auto parsedPort = std::stoi(port);
        return parsedPort > 0 && parsedPort <= 65535 ? parsedPort : -1;
    } catch (...) {
        return -1;
    }
}

ParsedURL parseURL(std::string url) {
    ParsedURL result;
    auto schemeEnd = url.find("://");
    if (schemeEnd != std::string::npos) {
        result.scheme = common::StringUtils::getLower(url.substr(0, schemeEnd));
        url = url.substr(schemeEnd + 3);
    }
    auto pathStart = url.find_first_of("/?#");
    if (pathStart != std::string::npos) {
        url = url.substr(0, pathStart);
    }
    auto atPos = url.rfind('@');
    if (atPos != std::string::npos) {
        url = url.substr(atPos + 1);
    }
    if (url.starts_with('[')) {
        auto bracketEnd = url.find(']');
        if (bracketEnd != std::string::npos) {
            result.host = url.substr(1, bracketEnd - 1);
            if (bracketEnd + 1 < url.size() && url[bracketEnd + 1] == ':') {
                result.port = parsePort(url.substr(bracketEnd + 2));
            }
        }
        return result;
    }
    auto portPos = url.rfind(':');
    if (portPos != std::string::npos && url.find(':') == portPos) {
        result.host = url.substr(0, portPos);
        result.port = parsePort(url.substr(portPos + 1));
    } else {
        result.host = url;
    }
    return result;
}

bool noProxyMatches(std::string noProxy, const ParsedURL& target) {
    auto targetHost = common::StringUtils::getLower(target.host);
    while (!noProxy.empty()) {
        auto commaPos = noProxy.find(',');
        auto entry = commaPos == std::string::npos ? noProxy : noProxy.substr(0, commaPos);
        entry = common::StringUtils::ltrim(common::StringUtils::rtrim(entry));
        entry = common::StringUtils::getLower(entry);
        if (entry == "*") {
            return true;
        }
        if (entry.starts_with('.')) {
            if (targetHost.ends_with(entry)) {
                return true;
            }
        } else {
            auto entryHost = parseURL(entry).host;
            entryHost = common::StringUtils::getLower(entryHost);
            if (targetHost == entryHost || targetHost.ends_with("." + entryHost)) {
                return true;
            }
        }
        if (commaPos == std::string::npos) {
            break;
        }
        noProxy = noProxy.substr(commaPos + 1);
    }
    return false;
}

} // namespace

std::string getOS() {
    std::string os = "linux";
#if !defined(_GLIBCXX_USE_CXX11_ABI) || _GLIBCXX_USE_CXX11_ABI == 0
    if (os == "linux") {
        os = "linux_old";
    }
#endif
#ifdef _WIN32
    os = "win";
#elif defined(__APPLE__)
    os = "osx";
#endif
    return os;
}

std::string getArch() {
    std::string arch = "amd64";
#if defined(__i386__) || defined(_M_IX86)
    arch = "x86";
#elif defined(__aarch64__) || defined(__ARM_ARCH_ISA_A64)
    arch = "arm64";
#endif
    return arch;
}

std::string getPlatform() {
    return getOS() + "_" + getArch();
}

static ExtensionRepoInfo getExtensionRepoInfo(const std::string& extensionURL) {
    auto parsedURL = parseURL(extensionURL);
    auto scheme = parsedURL.scheme.empty() ? "http" : parsedURL.scheme;
    auto hostStart = extensionURL.find("://");
    hostStart = hostStart == std::string::npos ? 0 : hostStart + 3;
    auto hostPathStart = extensionURL.find('/', hostStart);
    auto hostName = hostPathStart == std::string::npos ?
                        extensionURL.substr(hostStart) :
                        extensionURL.substr(hostStart, hostPathStart - hostStart);
    auto hostURL = std::format("{}://{}", scheme, hostName);
    auto hostPath = hostPathStart == std::string::npos ? "/" : extensionURL.substr(hostPathStart);
    return {hostPath, hostURL, extensionURL};
}

std::string ExtensionSourceUtils::toString(ExtensionSource source) {
    switch (source) {
    case ExtensionSource::OFFICIAL:
        return "OFFICIAL";
    case ExtensionSource::USER:
        return "USER";
    case ExtensionSource::STATIC_LINKED:
        return "STATIC LINK";
    default:
        UNREACHABLE_CODE;
    }
}

static ExtensionRepoInfo getExtensionFilePath(const std::string& extensionName,
    const std::string& extensionRepo, const std::string& fileName) {
    auto extensionURL = std::format(ExtensionUtils::EXTENSION_FILE_REPO_PATH, extensionRepo,
        LBUG_EXTENSION_VERSION, getPlatform(), extensionName, fileName);
    return getExtensionRepoInfo(extensionURL);
}

ExtensionRepoInfo ExtensionUtils::getExtensionLibRepoInfo(const std::string& extensionName,
    const std::string& extensionRepo) {
    return getExtensionFilePath(extensionName, extensionRepo, getExtensionFileName(extensionName));
}

ExtensionRepoInfo ExtensionUtils::getExtensionLoaderRepoInfo(const std::string& extensionName,
    const std::string& extensionRepo) {
    return getExtensionFilePath(extensionName, extensionRepo,
        getExtensionFileName(extensionName + EXTENSION_LOADER_SUFFIX));
}

ExtensionRepoInfo ExtensionUtils::getExtensionInstallerRepoInfo(const std::string& extensionName,
    const std::string& extensionRepo) {
    return getExtensionFilePath(extensionName, extensionRepo,
        getExtensionFileName(extensionName + EXTENSION_INSTALLER_SUFFIX));
}

ExtensionRepoInfo ExtensionUtils::getSharedLibRepoInfo(const std::string& fileName,
    const std::string& extensionRepo) {
    auto extensionURL = std::format(SHARED_LIB_REPO, extensionRepo, LBUG_EXTENSION_VERSION,
        getPlatform(), fileName);
    return getExtensionRepoInfo(extensionURL);
}

std::optional<ExtensionProxyConfig> ExtensionUtils::parseProxyConfig(const std::string& proxyURL) {
    auto parsedURL = parseURL(proxyURL);
    if (parsedURL.host.empty()) {
        return std::nullopt;
    }
    ExtensionProxyConfig config{parsedURL.host, parsedURL.port == -1 ? 80 : parsedURL.port, "", ""};
    auto authority = proxyURL;
    auto schemeEnd = authority.find("://");
    if (schemeEnd != std::string::npos) {
        authority = authority.substr(schemeEnd + 3);
    }
    auto pathStart = authority.find_first_of("/?#");
    if (pathStart != std::string::npos) {
        authority = authority.substr(0, pathStart);
    }
    auto atPos = authority.rfind('@');
    if (atPos != std::string::npos) {
        auto userInfo = authority.substr(0, atPos);
        auto passwordPos = userInfo.find(':');
        if (passwordPos == std::string::npos) {
            config.username = userInfo;
        } else {
            config.username = userInfo.substr(0, passwordPos);
            config.password = userInfo.substr(passwordPos + 1);
        }
    }
    return config;
}

std::optional<ExtensionProxyConfig> ExtensionUtils::getProxyConfigForURL(const std::string& url) {
    auto targetURL = parseURL(url);
    if (targetURL.host.empty()) {
        return std::nullopt;
    }
    auto noProxy = getProxyEnv({"LADYBUG_NO_PROXY", "no_proxy", "NO_PROXY"});
    if (!noProxy.empty() && noProxyMatches(noProxy, targetURL)) {
        return std::nullopt;
    }
    std::string proxyURL;
    if (targetURL.scheme == "https") {
        proxyURL = getProxyEnv({"LADYBUG_HTTPS_PROXY", "https_proxy", "HTTPS_PROXY"});
    } else {
        proxyURL = getProxyEnv({"LADYBUG_HTTP_PROXY", "http_proxy", "HTTP_PROXY"});
    }
    if (proxyURL.empty()) {
        proxyURL = getProxyEnv({"LADYBUG_ALL_PROXY", "all_proxy", "ALL_PROXY"});
    }
    return proxyURL.empty() ? std::nullopt : parseProxyConfig(proxyURL);
}

std::string ExtensionUtils::getExtensionFileName(const std::string& name) {
    return std::format(EXTENSION_FILE_NAME, common::StringUtils::getLower(name),
        EXTENSION_FILE_SUFFIX);
}

std::string ExtensionUtils::getLocalPathForExtensionLib(main::ClientContext* context,
    const std::string& extensionName) {
    return std::format("{}/{}", getLocalDirForExtension(context, extensionName),
        getExtensionFileName(extensionName));
}

std::string ExtensionUtils::getLocalPathForExtensionLoader(main::ClientContext* context,
    const std::string& extensionName) {
    return std::format("{}/{}", getLocalDirForExtension(context, extensionName),
        getExtensionFileName(extensionName + EXTENSION_LOADER_SUFFIX));
}

std::string ExtensionUtils::getLocalPathForExtensionInstaller(main::ClientContext* context,
    const std::string& extensionName) {
    return std::format("{}/{}", getLocalDirForExtension(context, extensionName),
        getExtensionFileName(extensionName + EXTENSION_INSTALLER_SUFFIX));
}

std::string ExtensionUtils::getLocalDirForExtension(main::ClientContext* context,
    const std::string& extensionName) {
    return std::format("{}{}", context->getExtensionDir(), extensionName);
}

std::string ExtensionUtils::appendLibSuffix(const std::string& libName) {
    auto os = getOS();
    std::string suffix;
    if (os == "linux" || os == "linux_old") {
        suffix = "so";
    } else if (os == "osx") {
        suffix = "dylib";
    } else {
        UNREACHABLE_CODE;
    }
    return std::format("{}.{}", libName, suffix);
}

std::string ExtensionUtils::getLocalPathForSharedLib(main::ClientContext* context,
    const std::string& libName) {
    return std::format("{}common/{}", context->getExtensionDir(), libName);
}

std::string ExtensionUtils::getLocalPathForSharedLib(main::ClientContext* context) {
    return std::format("{}common/", context->getExtensionDir());
}

bool ExtensionUtils::isOfficialExtension(const std::string& extension) {
    auto extensionUpperCase = common::StringUtils::getUpper(extension);
    for (auto& officialExtension : OFFICIAL_EXTENSION) {
        if (officialExtension == extensionUpperCase) {
            return true;
        }
    }
    return false;
}

void ExtensionUtils::registerIndexType(main::Database& database, storage::IndexType type) {
    database.getStorageManager()->registerIndexType(std::move(type));
}

ExtensionLibLoader::ExtensionLibLoader(const std::string& extensionName, const std::string& path)
    : extensionName{extensionName} {
    libHdl = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (libHdl == nullptr) {
        throw common::IOException(
            std::format("Failed to load library: {} which is needed by extension: {}.\nError: {}.",
                path, extensionName, common::dlErrMessage()));
    }
}

ext_load_func_t ExtensionLibLoader::getLoadFunc() {
    return (ext_load_func_t)getDynamicLibFunc(EXTENSION_LOAD_FUNC_NAME);
}

ext_init_func_t ExtensionLibLoader::getInitFunc() {
    return (ext_init_func_t)getDynamicLibFunc(EXTENSION_INIT_FUNC_NAME);
}

ext_name_func_t ExtensionLibLoader::getNameFunc() {
    return (ext_name_func_t)getDynamicLibFunc(EXTENSION_NAME_FUNC_NAME);
}

ext_install_func_t ExtensionLibLoader::getInstallFunc() {
    return (ext_install_func_t)getDynamicLibFunc(EXTENSION_INSTALL_FUNC_NAME);
}

void ExtensionLibLoader::unload() {
    DASSERT(libHdl != nullptr);
    dlclose(libHdl);
    libHdl = nullptr;
}

void* ExtensionLibLoader::getDynamicLibFunc(const std::string& funcName) {
    DASSERT(libHdl != nullptr);
    auto sym = dlsym(libHdl, funcName.c_str());
    if (sym == nullptr) {
        throw common::IOException(
            std::format("Failed to load {} function in extension {}.\nError: {}", funcName,
                extensionName, common::dlErrMessage()));
    }
    return sym;
}

#ifdef _WIN32
std::wstring utf8ToUnicode(const char* input) {
    uint32_t result;

    result = MultiByteToWideChar(CP_UTF8, 0, input, -1, nullptr, 0);
    if (result == 0) {
        throw common::IOException("Failure in MultiByteToWideChar");
    }
    auto buffer = std::make_unique<wchar_t[]>(result);
    result = MultiByteToWideChar(CP_UTF8, 0, input, -1, buffer.get(), result);
    if (result == 0) {
        throw common::IOException("Failure in MultiByteToWideChar");
    }
    return std::wstring(buffer.get(), result);
}

void* dlopen(const char* file, int /*mode*/) {
    DASSERT(file);
    auto fpath = utf8ToUnicode(file);
    return (void*)LoadLibraryW(fpath.c_str());
}

void* dlsym(void* handle, const char* name) {
    DASSERT(handle);
    return (void*)GetProcAddress((HINSTANCE)handle, name);
}

void dlclose(void* handle) {
    DASSERT(handle);
    FreeLibrary((HINSTANCE)handle);
}
#endif

} // namespace extension
} // namespace lbug

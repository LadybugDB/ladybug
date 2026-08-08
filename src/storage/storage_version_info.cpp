#include "storage/storage_version_info.h"

#include "common/exception/runtime.h"
#include <format>

namespace lbug {
namespace storage {

static std::string normalizeVersionForStorageLookup(std::string version) {
    auto numDots = 0u;
    for (auto i = 0u; i < version.length(); ++i) {
        if (version[i] == '.' && ++numDots == 3) {
            return version.substr(0, i);
        }
    }
    return version;
}

static bool usesStorageVersion40(const std::string& version) {
    return version.starts_with("0.12.") || version.starts_with("0.13.") ||
           version.starts_with("0.14.") || version.starts_with("0.15.") ||
           version.starts_with("0.16.");
}

storage_version_t StorageVersionInfo::getStorageVersionForVersionString(
    const std::string& rawVersion) {
    auto storageVersionInfo = getStorageVersionInfo();
    auto version = normalizeVersionForStorageLookup(rawVersion);
    if (usesStorageVersion40(version)) {
        return STORAGE_VERSION_40;
    }
    if (!storageVersionInfo.contains(version)) {
        throw common::RuntimeException(
            std::format("Lbug version '{}' has no storage version mapping. This is a build "
                        "configuration error: add the release to "
                        "StorageVersionInfo::getStorageVersionInfo().",
                rawVersion));
    }
    return storageVersionInfo.at(version);
}

storage_version_t StorageVersionInfo::getStorageVersion() {
    return getStorageVersionForVersionString(LBUG_CMAKE_VERSION);
}

} // namespace storage
} // namespace lbug

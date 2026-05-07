#include "storage/storage_version_info.h"

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

storage_version_t StorageVersionInfo::getStorageVersion() {
    auto storageVersionInfo = getStorageVersionInfo();
    auto version = normalizeVersionForStorageLookup(LBUG_CMAKE_VERSION);
    if (usesStorageVersion40(version)) {
        return STORAGE_VERSION_40;
    }
    if (!storageVersionInfo.contains(version)) {
        // If the current LBUG_CMAKE_VERSION is not in the map,
        // then we must run the newest version of lbug
        // LCOV_EXCL_START
        storage_version_t maxVersion = 0;
        for (auto& [_, versionNumber] : storageVersionInfo) {
            maxVersion = std::max(maxVersion, versionNumber);
        }
        return maxVersion;
        // LCOV_EXCL_STOP
    }
    return storageVersionInfo.at(version);
}

} // namespace storage
} // namespace lbug

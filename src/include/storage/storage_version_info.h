#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "common/api.h"

namespace lbug {
namespace storage {

using storage_version_t = uint64_t;

struct StorageVersionInfo {
    // Storage version 40 spans the releases after 0.11.0 where the on-disk catalog/data format did
    // not change.
    static constexpr storage_version_t STORAGE_VERSION_40 = 40;
    // Storage version 41 adds the table storage FORMAT field to catalog entries (enum encoding).
    static constexpr storage_version_t STORAGE_VERSION_41 = 41;

    static std::unordered_map<std::string, storage_version_t> getStorageVersionInfo() {
        return {{"0.12.0", STORAGE_VERSION_40}, {"0.12.2", STORAGE_VERSION_40},
            {"0.13.0", STORAGE_VERSION_40}, {"0.13.1", STORAGE_VERSION_40},
            {"0.14.0", STORAGE_VERSION_40}, {"0.14.1", STORAGE_VERSION_40},
            {"0.15.0", STORAGE_VERSION_40}, {"0.15.1", STORAGE_VERSION_40},
            {"0.15.2", STORAGE_VERSION_40}, {"0.15.3", STORAGE_VERSION_40},
            {"0.15.4", STORAGE_VERSION_40}, {"0.16.0", STORAGE_VERSION_40},
            {"0.16.1", STORAGE_VERSION_40}, {"0.17.0", STORAGE_VERSION_41}};
    }

    static LBUG_API storage_version_t getStorageVersion();
    static bool canReadStorageVersion(storage_version_t storageVersion) {
        return storageVersion == STORAGE_VERSION_40 || storageVersion == getStorageVersion();
    }

    static constexpr const char* MAGIC_BYTES = "LBUG";
};

} // namespace storage
} // namespace lbug

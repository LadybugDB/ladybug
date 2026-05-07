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

    static std::unordered_map<std::string, storage_version_t> getStorageVersionInfo() {
        return {{"0.12.0", STORAGE_VERSION_40}, {"0.12.2", STORAGE_VERSION_40},
            {"0.13.0", STORAGE_VERSION_40}, {"0.13.1", STORAGE_VERSION_40},
            {"0.14.0", STORAGE_VERSION_40}, {"0.14.1", STORAGE_VERSION_40},
            {"0.15.0", STORAGE_VERSION_40}, {"0.15.1", STORAGE_VERSION_40},
            {"0.15.2", STORAGE_VERSION_40}, {"0.15.3", STORAGE_VERSION_40},
            {"0.15.4", STORAGE_VERSION_40}, {"0.16.0", STORAGE_VERSION_40},
            {"0.16.1", STORAGE_VERSION_40}};
    }

    static LBUG_API storage_version_t getStorageVersion();

    static constexpr const char* MAGIC_BYTES = "LBUG";
};

} // namespace storage
} // namespace lbug

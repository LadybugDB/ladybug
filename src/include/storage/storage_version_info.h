#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "common/api.h"

namespace lbug {
namespace storage {

using storage_version_t = uint64_t;

struct StorageVersionInfo {
    static std::unordered_map<std::string, storage_version_t> getStorageVersionInfo() {
        return {{"0.12.0", 40}, {"0.12.2", 40}, {"0.13.0", 40}, {"0.13.1", 40}, {"0.14.0", 40},
            {"0.14.1", 40}, {"0.15.0", 40}, {"0.15.1", 40}, {"0.15.2", 40}, {"0.15.3", 40},
            {"0.15.4", 40}};
    }

    static LBUG_API storage_version_t getStorageVersion();

    static constexpr const char* MAGIC_BYTES = "LBUG";
};

} // namespace storage
} // namespace lbug

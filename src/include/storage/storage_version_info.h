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
    // Storage version 42 adds per-FROM/TO relationship multiplicity to rel table catalog info.
    static constexpr storage_version_t STORAGE_VERSION_42 = 42;
    // Storage version 43 adds optional node-table sorted-by catalog metadata.
    static constexpr storage_version_t STORAGE_VERSION_43 = 43;
    // Storage version 44 adds the optional CSR (csr_index) flag and its changeEpoch watermark to
    // node-table sorted-by catalog metadata.
    static constexpr storage_version_t STORAGE_VERSION_44 = 44;
    // Storage version 45 adds the optional CSR sorted-by-dest flag and its changeEpoch watermark
    // to rel-table (rel group) catalog metadata.
    static constexpr storage_version_t STORAGE_VERSION_45 = 45;
    // Storage version 46 adds PostgreSQL-style table partitioning metadata (RANGE/HASH partition
    // key + per-partition subgraph node-table back-references) to node table catalog entries.
    static constexpr storage_version_t STORAGE_VERSION_46 = 46;
    // Storage version 47 adds LIST partitioning: the parent node-table entry persists the
    // encoded-key -> child table id map for its dynamically created list partitions.
    static constexpr storage_version_t STORAGE_VERSION_47 = 47;

    static std::unordered_map<std::string, storage_version_t> getStorageVersionInfo() {
        return {{"0.12.0", STORAGE_VERSION_40}, {"0.12.2", STORAGE_VERSION_40},
            {"0.13.0", STORAGE_VERSION_40}, {"0.13.1", STORAGE_VERSION_40},
            {"0.14.0", STORAGE_VERSION_40}, {"0.14.1", STORAGE_VERSION_40},
            {"0.15.0", STORAGE_VERSION_40}, {"0.15.1", STORAGE_VERSION_40},
            {"0.15.2", STORAGE_VERSION_40}, {"0.15.3", STORAGE_VERSION_40},
            {"0.15.4", STORAGE_VERSION_40}, {"0.16.0", STORAGE_VERSION_40},
            {"0.16.1", STORAGE_VERSION_40}, {"0.17.0", STORAGE_VERSION_41},
            {"0.17.1", STORAGE_VERSION_41}, {"0.18.0", STORAGE_VERSION_42},
            {"0.18.1", STORAGE_VERSION_42}, {"0.19.0", STORAGE_VERSION_43},
            {"0.19.1", STORAGE_VERSION_43}, {"0.20.0", STORAGE_VERSION_47},
            {"0.20.1", STORAGE_VERSION_47}, {"0.20.2", STORAGE_VERSION_47}};
    }

    static LBUG_API storage_version_t getStorageVersion();
    static bool canReadStorageVersion(storage_version_t storageVersion) {
        return storageVersion == STORAGE_VERSION_40 || storageVersion == STORAGE_VERSION_41 ||
               storageVersion == STORAGE_VERSION_42 || storageVersion == STORAGE_VERSION_43 ||
               storageVersion == STORAGE_VERSION_44 || storageVersion == STORAGE_VERSION_45 ||
               storageVersion == getStorageVersion();
    }

    static constexpr const char* MAGIC_BYTES = "LBUG";
};

} // namespace storage
} // namespace lbug

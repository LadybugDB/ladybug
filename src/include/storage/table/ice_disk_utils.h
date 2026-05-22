#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "common/constants.h"
#include "common/exception/runtime.h"
#include "common/file_system/virtual_file_system.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"
#include "storage/table/ice_disk_constants.h"

namespace lbug {
namespace storage {

struct CSRFilePaths {
    std::string indices;
    std::string indptr;
};

class IceDiskUtils {
public:
    // Joins a base path with a filename. When base is empty the filename is returned
    // as-is (i.e. relative to the current working directory)
    // public for testing
    static std::string joinPath(const std::string& base, const std::string& part) {
        if (base.empty()) {
            return part;
        }
        const char last = base.back();
        if (last == '/' || last == '\\') {
            return base + part;
        }

        return base + "/" + part;
    }

    // Get the file path for a given node table's parquet file
    static std::string constructNodeTablePath(const std::string& dir, const std::string& name,
        const std::string& suffix) {
        return IceDiskUtils::joinPath(dir, "nodes_" + name + suffix);
    }

    // Get the file paths for a given rel table's CSR files
    static CSRFilePaths constructCSRPaths(const std::string& dir, const std::string& name,
        const std::string& suffix) {
        return {IceDiskUtils::joinPath(dir, "indices_" + name + suffix),
            IceDiskUtils::joinPath(dir, "indptr_" + name + suffix)};
    }

    // Get the file path for a flat relationship table. The file contains source and target node
    // offsets followed by relationship property columns.
    static std::string constructFlatRelTablePath(const std::string& dir, const std::string& name,
        const std::string& suffix) {
        return IceDiskUtils::joinPath(dir, "rels_" + name + suffix);
    }

    // Validates that the parquet file at `path` carries the expected icebug_disk_version metadata.
    // Note: path is already resolved by VFS
    static void checkVersionCompatibility(main::ClientContext* context, const std::string& path) {
        if (!context) {
            throw common::RuntimeException(
                path + ": failed to read parquet metadata for version check");
        }

        auto tempReader =
            std::make_unique<processor::ParquetReader>(path, std::vector<bool>{}, context);
        if (!tempReader) {
            throw common::RuntimeException(
                path + ": failed to read parquet metadata for version check");
        }

        auto metadata = tempReader->getMetadata();
        if (!metadata) {
            throw common::RuntimeException(
                path + ": failed to read parquet metadata for version check");
        }

        // key_value_metadata is std::vector<KeyValue>
        bool found = false;
        std::string versionValue;
        for (const auto& kv : metadata->key_value_metadata) {
            if (kv.key == IceDiskConstants::VERSION_METADATA_KEY) {
                versionValue = kv.value;
                found = true;
                break;
            }
        }

        if (!found) {
            std::cerr << "Warning: " << path
                      << ": parquet file is missing icebug_disk_version metadata; continuing "
                         "without version compatibility check"
                      << std::endl;
            return;
        }

        const auto& expected = IceDiskConstants::CURRENT_VERSION;
        const bool versionMatches = versionValue.size() == expected.size() &&
                                    std::equal(versionValue.begin(), versionValue.end(),
                                        expected.begin(), [](unsigned char a, unsigned char b) {
                                            return std::tolower(a) == std::tolower(b);
                                        });

        if (!versionMatches) {
            throw common::RuntimeException(
                path +
                ": current ladybug version does not support icebug_disk_version: " + versionValue);
        }
    }
};

} // namespace storage
} // namespace lbug

#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
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
    // Parses "icebug-disk", "icebug-disk:", or "icebug-disk:<path>" and returns the path
    // component. Returns empty string for the first two forms (caller interprets as current dir)
    static std::string getBasePath(const std::string& storage) {
        if (!storage.starts_with(common::TableOptionConstants::ICEBUG_DISK_PREFIX)) {
            return "";
        }
        std::string_view rest = std::string_view(storage).substr(
            common::TableOptionConstants::ICEBUG_DISK_PREFIX.size());
        // Strip the optional ':' separator.
        if (!rest.empty() && rest[0] == ':') {
            rest = rest.substr(1);
        }
        return std::string(rest); // empty means "current directory"
    }

    // Joins a base path with a filename. When base is empty the filename is returned
    // as-is (i.e. relative to the current working directory)
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

    // Resolves a potentially relative or ~-prefixed path against dbDirectory.
    static std::string resolveIceDiskPath(const std::string& path, const std::string& dbDirectory) {
        std::string expanded = path;
        if (!expanded.empty() && expanded[0] == '~') {
            const char* home = std::getenv("HOME");
            if (!home) {
                throw common::RuntimeException(
                    "Cannot expand '~' in IceDisk path without HOME set: " + path);
            }
            expanded = std::string(home) + expanded.substr(1);
        }
        if (!std::filesystem::path(expanded).is_absolute()) {
            expanded = (std::filesystem::path(dbDirectory) / expanded).lexically_normal().string();
        }
        return expanded;
    }

    // Validates that the parquet file at `path` carries the expected icebug_disk_version metadata.
    // `dbDirectory` is used to anchor relative paths (typically parent of the .ladybug file).
    static void checkVersionCompatibility(common::VirtualFileSystem* vfs,
        const std::string& dbDirectory, const std::string& path) {
        if (!vfs) {
            throw common::RuntimeException(
                "No VirtualFileSystem available for IceDisk version check: " + path);
        }

        const auto resolvedPath = resolveIceDiskPath(path, dbDirectory);

        auto metadata = processor::ParquetReader::readMetadata(resolvedPath, vfs);
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
            throw common::RuntimeException(
                path + ": parquet file is missing icebug_disk_version metadata");
        }

        const auto& expected = IceDiskConstants::CURRENT_VERSION;
        const bool versionMatches = versionValue.size() == expected.size() &&
                                    std::equal(versionValue.begin(), versionValue.end(),
                                        expected.begin(), [](unsigned char a, unsigned char b) {
                                            return std::tolower(a) == std::tolower(b);
                                        });

        if (!versionMatches) {
            throw common::RuntimeException(
                "Current ladybug version does not support icebug_disk_version: " + versionValue);
        }
    }
};

} // namespace storage
} // namespace lbug

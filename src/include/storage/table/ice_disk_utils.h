#pragma once

#include <string>

#include "common/constants.h"

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
};

} // namespace storage
} // namespace lbug

#pragma once

#include <cstdint>
#include <string>

namespace lbug {
namespace common {

// Storage format of a node/rel table. Native tables use NONE. ICEBUG_DISK is the
// in-core columnar format. Any other columnar backend (e.g. lance, delta) lives in
// an extension and is identified by EXTERNAL plus a format name string
// (NodeTableCatalogEntry::getStorageFormatName); core never enumerates those names.
// Extensions claim names through ColumnarTableHooks::formatName/canHandle.
// Do not add per-format enumerators here: extension formats are identified by
// EXTERNAL plus a format name through the columnar table hook registry instead.
enum class StorageFormat : uint8_t { NONE, ICEBUG_DISK, EXTERNAL };

// Binder-level parse of WITH (format = '...'): the enum plus the external format
// name (lowercased, empty unless the format is EXTERNAL).
struct ParsedStorageFormat {
    StorageFormat format = StorageFormat::NONE;
    std::string externalName;
};

struct StorageFormatUtils {
    // Parses a WITH (format = '...') option. 'icebug-disk' maps to ICEBUG_DISK; any
    // other non-empty value maps to EXTERNAL with that value as the external name, so
    // core accepts extension formats without naming them. Unknown names fail later at
    // table creation with a 'load the extension' error, not here.
    static ParsedStorageFormat parse(const std::string& str);
    static StorageFormat fromString(const std::string& str);
    static std::string toString(StorageFormat format);
    // Display name for errors/DDL round-tripping: 'icebug-disk' for ICEBUG_DISK, the
    // external name for EXTERNAL (or 'external' when no name is known).
    static std::string displayName(StorageFormat format, const std::string& externalName = "");
    // Lowercase-normalized external format name used for registry lookup and catalog
    // persistence.
    static std::string normalizeExternalName(const std::string& name);
    // True for any on-disk columnar format (in-core or extension-backed).
    static bool isColumnar(StorageFormat format) { return format != StorageFormat::NONE; }
    // Format identity for the CREATE REL TABLE no-mixing rule: EXTERNAL formats only
    // match when their external names are equal (case-insensitive).
    static bool equals(StorageFormat lhs, const std::string& lhsName, StorageFormat rhs,
        const std::string& rhsName);
};

} // namespace common
} // namespace lbug

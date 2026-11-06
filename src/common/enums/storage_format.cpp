#include "common/enums/storage_format.h"

#include "common/string_utils.h"

namespace lbug {
namespace common {

std::string StorageFormatUtils::normalizeExternalName(const std::string& name) {
    return StringUtils::getLower(name);
}

ParsedStorageFormat StorageFormatUtils::parse(const std::string& str) {
    if (StringUtils::caseInsensitiveEquals(str, "icebug-disk")) {
        return ParsedStorageFormat{StorageFormat::ICEBUG_DISK, ""};
    }
    // Any other value names an extension-provided columnar format. Core does not
    // validate it here; StorageManager reports a 'load the extension' error if no
    // extension has registered the name.
    return ParsedStorageFormat{StorageFormat::EXTERNAL, normalizeExternalName(str)};
}

StorageFormat StorageFormatUtils::fromString(const std::string& str) {
    return parse(str).format;
}

std::string StorageFormatUtils::toString(StorageFormat format) {
    switch (format) {
    case StorageFormat::NONE: {
        return "none";
    }
    case StorageFormat::ICEBUG_DISK: {
        return "icebug-disk";
    }
    case StorageFormat::EXTERNAL: {
        return "external";
    }
    default: {
        return "unknown";
    }
    }
}

std::string StorageFormatUtils::displayName(StorageFormat format, const std::string& externalName) {
    if (format == StorageFormat::EXTERNAL) {
        return externalName.empty() ? "external" : normalizeExternalName(externalName);
    }
    return toString(format);
}

bool StorageFormatUtils::equals(StorageFormat lhs, const std::string& lhsName, StorageFormat rhs,
    const std::string& rhsName) {
    if (lhs != rhs) {
        return false;
    }
    if (lhs == StorageFormat::EXTERNAL) {
        return normalizeExternalName(lhsName) == normalizeExternalName(rhsName);
    }
    return true;
}

} // namespace common
} // namespace lbug

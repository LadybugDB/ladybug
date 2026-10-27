#include "common/enums/storage_format.h"

#include "common/exception/binder.h"
#include <format>

namespace lbug {
namespace common {

StorageFormat StorageFormatUtils::fromString(const std::string& str) {
    if (str == "icebug-disk") {
        return StorageFormat::ICEBUG_DISK;
    }
    if (str == "lance") {
        return StorageFormat::LANCE;
    }
    throw BinderException(std::format(
        "Unsupported storage format '{}'. Valid options are: icebug-disk, lance.", str));
}

std::string StorageFormatUtils::toString(StorageFormat format) {
    switch (format) {
    case StorageFormat::NONE: {
        return "none";
    }
    case StorageFormat::ICEBUG_DISK: {
        return "icebug-disk";
    }
    case StorageFormat::LANCE: {
        return "lance";
    }
    default: {
        return "unknown";
    }
    }
}

} // namespace common
} // namespace lbug

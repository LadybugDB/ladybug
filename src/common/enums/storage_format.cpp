#include "common/enums/storage_format.h"

#include "common/exception/binder.h"
#include <format>

namespace lbug {
namespace common {

StorageFormat StorageFormatUtils::fromString(const std::string& str) {
    if (str == "icebug-disk") {
        return StorageFormat::ICEBUG_DISK;
    }
    throw BinderException(
        std::format("Unsupported storage format '{}'. Valid options are: icebug-disk.", str));
}

std::string StorageFormatUtils::toString(StorageFormat format) {
    switch (format) {
    case StorageFormat::NONE:
        return "";
    case StorageFormat::ICEBUG_DISK:
        return "icebug-disk";
    default:
        return "";
    }
}

} // namespace common
} // namespace lbug

#include "common/enums/table_storage_format.h"

#include "common/assert.h"
#include "common/exception/binder.h"
#include "common/string_utils.h"
#include <format>

namespace lbug {
namespace common {

std::string TableStorageFormatUtils::toString(TableStorageFormat format) {
    switch (format) {
    case TableStorageFormat::UNKNOWN:
        return "unknown";
    case TableStorageFormat::NATIVE:
        return "native";
    case TableStorageFormat::ICEBUG_DISK:
        return "icebug-disk";
    case TableStorageFormat::ARROW:
        return "arrow";
    case TableStorageFormat::FOREIGN:
        return "foreign";
    default:
        UNREACHABLE_CODE;
    }
}

bool TableStorageFormatUtils::isArrow(TableStorageFormat format) {
    return format == TableStorageFormat::ARROW;
}

bool TableStorageFormatUtils::isIceDisk(TableStorageFormat format) {
    return format == TableStorageFormat::ICEBUG_DISK;
}

} // namespace common
} // namespace lbug

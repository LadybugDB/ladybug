#pragma once

#include <cstdint>
#include <string>

#include "common/api.h"

namespace lbug {
namespace common {

enum class TableStorageFormat : uint8_t {
    UNKNOWN = 0,
    // native
    NATIVE = 1,
    // first class citizens
    ICEBUG_DISK = 10,
    ARROW = 11,
    // foreign tables
    FOREIGN = 30,
};

struct TableStorageFormatUtils {
    static std::string toString(TableStorageFormat format);
    static bool isArrow(TableStorageFormat format);
    static bool isIceDisk(TableStorageFormat format);
};

} // namespace common
} // namespace lbug

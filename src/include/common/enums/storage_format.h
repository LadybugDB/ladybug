#pragma once

#include <cstdint>
#include <string>

namespace lbug {
namespace common {

enum class StorageFormat : uint8_t {
    NONE = 0,
    // first class citizens
    ICEBUG_DISK = 1,
    // external formats
    EXTERNAL = 20
};

struct StorageFormatUtils {
    static StorageFormat fromString(const std::string& str);
};

} // namespace common
} // namespace lbug

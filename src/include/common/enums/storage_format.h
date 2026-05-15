#pragma once

#include <cstdint>
#include <string>

namespace lbug {
namespace common {

enum class StorageFormat : uint8_t { NONE, ICEBUG_DISK };

struct StorageFormatUtils {
    static StorageFormat fromString(const std::string& str);
    static std::string toString(StorageFormat format);
};

} // namespace common
} // namespace lbug

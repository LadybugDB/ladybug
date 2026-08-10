#pragma once

#include <cstdint>

namespace lbug {
namespace function {

enum class ComparisonOperation : uint8_t {
    EQUAL,
    NOT_EQUAL,
    GREATER_THAN,
    GREATER_THAN_EQUAL,
    LESS_THAN,
    LESS_THAN_EQUAL,
};

} // namespace function
} // namespace lbug

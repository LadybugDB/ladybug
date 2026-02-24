#pragma once

#include "common/types/string_t.h"

namespace lbug {
namespace function {

struct StartsWith {
    static inline void operation(common::string_t& left, common::string_t& right,
        uint8_t& result) {
        auto lStr = left.getAsString();
        auto rStr = right.getAsString();
        result = lStr.starts_with(rStr);
    }
};

} // namespace function
} // namespace lbug

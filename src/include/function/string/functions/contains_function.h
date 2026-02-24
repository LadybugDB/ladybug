#pragma once

#include "common/types/string_t.h"
#include "function/string/functions/find_function.h"

namespace lbug {
namespace function {

struct Contains {
    static inline void operation(common::string_t& left, common::string_t& right,
        uint8_t& result) {
        int64_t pos = 0;
        Find::operation(left, right, pos);
        result = (pos != 0);
    }
};

} // namespace function
} // namespace lbug

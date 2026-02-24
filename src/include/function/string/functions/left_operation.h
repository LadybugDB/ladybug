#pragma once

#include "common/types/string_t.h"
#include "function/list/functions/list_len_function.h"
#include "substr_function.h"

namespace lbug {
namespace function {

struct Left {
public:
    static inline void operation(common::string_t& left, int64_t& right, common::string_t& result,
        common::ValueVector& resultValueVector) {
        int64_t leftLen = 0;
        ListLen::operation(left, leftLen);
        int64_t len =
            (right > -1) ? std::min(leftLen, right) : std::max(leftLen + right, (int64_t)0);
        SubStr::operation(left, 1, len, result, resultValueVector);
    }
};

} // namespace function
} // namespace lbug

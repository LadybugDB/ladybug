#pragma once

#include "base_str_function.h"
#include "common/types/string_t.h"

namespace lbug {
namespace function {

struct Rtrim {
    static inline void operation(common::string_t& input, common::string_t& result,
        common::ValueVector& resultValueVector) {
        BaseStrOperation::operation(input, result, resultValueVector, rtrim);
    }

    static uint32_t rtrim(char* data, uint32_t len) {
        int32_t counter = len - 1;
        for (; counter >= 0; counter--) {
            if (!isspace(data[counter])) {
                break;
            }
        }
        return counter + 1;
    }
};

} // namespace function
} // namespace lbug

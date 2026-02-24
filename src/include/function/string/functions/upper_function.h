#pragma once

#include "common/types/string_t.h"
#include "common/vector/value_vector.h"
#include "function/string/functions/base_lower_upper_function.h"

namespace lbug {
namespace function {

struct Upper {
public:
    static inline void operation(common::string_t& input, common::string_t& result,
        common::ValueVector& resultValueVector) {
        BaseLowerUpperFunction::operation(input, result, resultValueVector, true /* isUpper */);
    }
};

} // namespace function
} // namespace lbug

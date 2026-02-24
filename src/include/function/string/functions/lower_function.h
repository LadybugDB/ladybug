#pragma once

#include "base_lower_upper_function.h"
#include "common/types/string_t.h"

namespace lbug {
namespace function {

struct Lower {
public:
    static inline void operation(common::string_t& input, common::string_t& result,
        common::ValueVector& resultValueVector) {
        BaseLowerUpperFunction::operation(input, result, resultValueVector, false /* isUpper */);
    }
};

} // namespace function
} // namespace lbug

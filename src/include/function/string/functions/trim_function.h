#pragma once

#include "common/types/string_t.h"
#include "ltrim_function.h"
#include "rtrim_function.h"

namespace lbug {
namespace function {

struct Trim : BaseStrOperation {
public:
    static inline void operation(common::string_t& input, common::string_t& result,
        common::ValueVector& resultValueVector) {
        BaseStrOperation::operation(input, result, resultValueVector, trim);
    }

private:
    static uint32_t trim(char* data, uint32_t len) {
        return Rtrim::rtrim(data, Ltrim::ltrim(data, len));
    }
};

} // namespace function
} // namespace lbug

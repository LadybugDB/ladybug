#pragma once

#include "common/vector/value_vector.h"

namespace lbug {
namespace function {

struct ConstFunctionExecutor {

    template<typename RESULT_TYPE, typename OP>
    static void execute(common::ValueVector& result, common::SelectionVector& sel) {
        DASSERT(result.state->isFlat());
        auto resultValues = (RESULT_TYPE*)result.getData();
        auto idx = sel[0];
        DASSERT(idx == 0);
        OP::operation(resultValues[idx]);
    }
};

} // namespace function
} // namespace lbug

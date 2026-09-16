#pragma once

#include "base_count.h"

namespace lbug {
namespace function {

struct CountFunction : public BaseCountFunction {
    static constexpr const char* name = "COUNT";

    static void updateAll(uint8_t* state_, common::ValueVector* input, uint64_t multiplicity,
        common::InMemOverflowBuffer* overflowBuffer);

    // NOLINTNEXTLINE(readability-non-const-parameter): Would cast away qualifiers.
    static inline void updatePos(uint8_t* state_, common::ValueVector* /*input*/,
        uint64_t multiplicity, uint32_t /*pos*/, common::InMemOverflowBuffer* /*overflowBuffer*/) {
        // state_ may point into a packed factorized-table tuple without alignment
        // padding; go through memcpy (see BaseCountFunction::addToCount).
        addToCount(state_, multiplicity);
    }

    static void paramRewriteFunc(binder::expression_vector& arguments);

    static function_set getFunctionSet();
};

} // namespace function
} // namespace lbug

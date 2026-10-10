#pragma once

#include <optional>

#include "binder/expression/expression.h"
#include "common/types/value/value.h"

namespace lbug {
namespace evaluator {

struct ExpressionEvaluatorUtils {
    // Results with more nested values (list elements, struct fields) are left to run-time
    // evaluation; a folded common::Value is far larger than the vector it comes from.
    static constexpr uint64_t MAX_FOLDED_NESTED_VALUES = 8192;

    // Evaluates a constant expression unless its result holds more than MAX_FOLDED_NESTED_VALUES.
    static std::optional<common::Value> tryEvaluateConstantExpression(
        std::shared_ptr<binder::Expression> expression, main::ClientContext* clientContext);

    static LBUG_API common::Value evaluateConstantExpression(
        std::shared_ptr<binder::Expression> expression, main::ClientContext* clientContext);
};

} // namespace evaluator
} // namespace lbug

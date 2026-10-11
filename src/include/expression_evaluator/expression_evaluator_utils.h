#pragma once

#include <optional>

#include "binder/expression/expression.h"
#include "common/types/value/value.h"

namespace lbug {
namespace evaluator {

struct ExpressionEvaluatorUtils {
    // A result with more nested values (list elements, struct fields) than this and than the
    // expression itself holds is left to run-time evaluation: a folded common::Value is far
    // larger than the vector it comes from. A list written out in the query is still folded.
    static constexpr uint64_t MAX_FOLDED_NESTED_VALUES = 8192;

    // Evaluates a constant expression unless the result is too large to fold (see above).
    static std::optional<common::Value> tryEvaluateConstantExpression(
        std::shared_ptr<binder::Expression> expression, main::ClientContext* clientContext);

    static LBUG_API common::Value evaluateConstantExpression(
        std::shared_ptr<binder::Expression> expression, main::ClientContext* clientContext);
};

} // namespace evaluator
} // namespace lbug

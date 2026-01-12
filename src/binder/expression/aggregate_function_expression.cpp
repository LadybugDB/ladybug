#include "binder/expression/aggregate_function_expression.h"

#include "binder/expression/expression_util.h"
#include <format>

using namespace lbug::common;

namespace lbug {
namespace binder {

std::string AggregateFunctionExpression::toStringInternal() const {
    return std::format("{}({}{})", function.name, function.isDistinct ? "DISTINCT " : "",
        ExpressionUtil::toString(children));
}

std::string AggregateFunctionExpression::getUniqueName(const std::string& functionName,
    const expression_vector& children, bool isDistinct) {
    return std::format("{}({}{})", functionName, isDistinct ? "DISTINCT " : "",
        ExpressionUtil::getUniqueName(children));
}

} // namespace binder
} // namespace lbug

#include "binder/expression/scalar_function_expression.h"

#include "binder/expression/expression_util.h"
#include "function/cast/cast_function_bind_data.h"
#include <format>

using namespace lbug::common;

namespace lbug {
namespace binder {

bool ScalarFunctionExpression::isCast() const {
    if (function == nullptr) {
        return false;
    }
    if (function->name != "CAST" && !function->name.starts_with("CAST_TO_")) {
        return false;
    }
    return dynamic_cast<const function::CastFunctionBindData*>(bindData.get()) != nullptr;
}

std::string ScalarFunctionExpression::toStringInternal() const {
    if (isCast()) {
        return std::format("CAST({}, {})", ExpressionUtil::toString(children),
            bindData->resultType.toString());
    }
    return std::format("{}({})", function->name, ExpressionUtil::toString(children));
}

std::string ScalarFunctionExpression::getUniqueName(const std::string& functionName,
    const expression_vector& children) {
    return std::format("{}({})", functionName, ExpressionUtil::getUniqueName(children));
}

} // namespace binder
} // namespace lbug

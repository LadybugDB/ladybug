#include "binder/expression/parameter_expression.h"
#include "binder/expression_binder.h"
#include "common/exception/binder.h"
#include "parser/expression/parsed_parameter_expression.h"
#include <format>

using namespace lbug::common;
using namespace lbug::parser;

namespace lbug {
namespace binder {

std::shared_ptr<Expression> ExpressionBinder::bindParameterExpression(
    const ParsedExpression& parsedExpression) {
    auto& parsedParameterExpression = parsedExpression.constCast<ParsedParameterExpression>();
    auto parameterName = parsedParameterExpression.getParameterName();
    if (knownParameters.contains(parameterName)) {
        auto bound =
            make_shared<ParameterExpression>(parameterName, knownParameters.at(parameterName));
        boundParameters.push_back(bound);
        return bound;
    }
    // LCOV_EXCL_START
    throw BinderException(
        std::format("Cannot find parameter {}. This should not happen.", parameterName));
    // LCOV_EXCL_STOP
}

} // namespace binder
} // namespace lbug

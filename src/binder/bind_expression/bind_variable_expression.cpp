#include "binder/binder.h"
#include "binder/expression/variable_expression.h"
#include "binder/expression_binder.h"
#include "common/exception/binder.h"
#include "common/exception/message.h"
#include "parser/expression/parsed_variable_expression.h"

using namespace lbug::common;
using namespace lbug::parser;

namespace lbug {
namespace binder {

std::shared_ptr<Expression> ExpressionBinder::bindVariableExpression(
    const ParsedExpression& parsedExpression) const {
    auto& variableExpression =
        dynamic_cast_checked<const ParsedVariableExpression&>(parsedExpression);
    auto variableName = variableExpression.getVariableName();
    return bindVariableExpression(variableName);
}

std::shared_ptr<Expression> ExpressionBinder::bindVariableExpression(
    const std::string& varName) const {
    if (binder->scope.contains(varName)) {
        // Return the scope's own expression object rather than a copy of it: callers identify
        // scope expressions by pointer identity. bind_projection_clause.cpp, for one, decides
        // that an aggregate below an in-scope expression is not nested by comparing raw pointers.
        return binder->scope.getExpression(varName);
    }
    throw BinderException(ExceptionMessage::variableNotInScope(varName));
}

std::shared_ptr<Expression> ExpressionBinder::createVariableExpression(LogicalType logicalType,
    std::string_view name) const {
    return createVariableExpression(std::move(logicalType), std::string(name));
}

std::shared_ptr<Expression> ExpressionBinder::createVariableExpression(LogicalType logicalType,
    std::string name) const {
    return std::make_shared<VariableExpression>(std::move(logicalType),
        binder->getUniqueExpressionName(name), std::move(name));
}

} // namespace binder
} // namespace lbug

#include "binder/expression_binder.h"

#include "binder/binder.h"
#include "binder/expression/case_expression.h"
#include "binder/expression/expression_util.h"
#include "binder/expression/literal_expression.h"
#include "binder/expression/parameter_expression.h"
#include "binder/expression/scalar_function_expression.h"
#include "binder/expression_visitor.h"
#include "common/exception/binder.h"
#include "common/exception/not_implemented.h"
#include "expression_evaluator/expression_evaluator_utils.h"
#include "function/arithmetic/vector_arithmetic_functions.h"
#include "function/cast/vector_cast_functions.h"
#include "parser/expression/parsed_expression_visitor.h"
#include "parser/expression/parsed_parameter_expression.h"
#include <format>

using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::parser;

namespace lbug {
namespace binder {

static bool isBoolLiteral(const Expression& expression, bool value) {
    if (expression.expressionType != ExpressionType::LITERAL ||
        expression.dataType != LogicalType::BOOL()) {
        return false;
    }
    auto literalValue = expression.constCast<LiteralExpression>().getValue();
    return !literalValue.isNull() && literalValue.getValue<bool>() == value;
}

// Checks whether the expression is a non-null integral literal with value 0.
// Restricted to integral types: for floating point, x + 0.0 can flip the sign of a
// signed zero (e.g. -0.0 + 0.0 == +0.0), so the rewrite would not be bit-preserving.
// Adding/subtracting integral zero can neither overflow nor change NULL semantics
// (NULL + 0 == NULL == x), so the identity is safe.
static bool isIntegralZeroLiteral(const Expression& expression) {
    if (expression.expressionType != ExpressionType::LITERAL) {
        return false;
    }
    auto& literal = expression.constCast<LiteralExpression>();
    if (literal.isNull()) {
        return false;
    }
    auto value = literal.getValue();
    switch (expression.dataType.getLogicalTypeID()) {
    case LogicalTypeID::INT8:
        return value.getValue<int8_t>() == 0;
    case LogicalTypeID::INT16:
        return value.getValue<int16_t>() == 0;
    case LogicalTypeID::INT32:
        return value.getValue<int32_t>() == 0;
    case LogicalTypeID::INT64:
    case LogicalTypeID::SERIAL:
        return value.getValue<int64_t>() == 0;
    case LogicalTypeID::UINT8:
        return value.getValue<uint8_t>() == 0;
    case LogicalTypeID::UINT16:
        return value.getValue<uint16_t>() == 0;
    case LogicalTypeID::UINT32:
        return value.getValue<uint32_t>() == 0;
    case LogicalTypeID::UINT64:
        return value.getValue<uint64_t>() == 0;
    case LogicalTypeID::INT128: {
        auto v = value.getValue<int128_t>();
        return v.low == 0 && v.high == 0;
    }
    case LogicalTypeID::UINT128: {
        auto v = value.getValue<uint128_t>();
        return v.low == 0 && v.high == 0;
    }
    default:
        return false;
    }
}

// Checks whether the expression is a non-null numeric literal with value 1.
// Multiplying/dividing by one preserves signed zeros, NaN and infinities, and cannot
// overflow, so the identity is safe for integral and floating point types alike.
// DECIMAL is excluded: its scaled representation makes the one-check scale-dependent.
static bool isNumericOneLiteral(const Expression& expression) {
    if (expression.expressionType != ExpressionType::LITERAL) {
        return false;
    }
    auto& literal = expression.constCast<LiteralExpression>();
    if (literal.isNull()) {
        return false;
    }
    auto value = literal.getValue();
    switch (expression.dataType.getLogicalTypeID()) {
    case LogicalTypeID::INT8:
        return value.getValue<int8_t>() == 1;
    case LogicalTypeID::INT16:
        return value.getValue<int16_t>() == 1;
    case LogicalTypeID::INT32:
        return value.getValue<int32_t>() == 1;
    case LogicalTypeID::INT64:
    case LogicalTypeID::SERIAL:
        return value.getValue<int64_t>() == 1;
    case LogicalTypeID::UINT8:
        return value.getValue<uint8_t>() == 1;
    case LogicalTypeID::UINT16:
        return value.getValue<uint16_t>() == 1;
    case LogicalTypeID::UINT32:
        return value.getValue<uint32_t>() == 1;
    case LogicalTypeID::UINT64:
        return value.getValue<uint64_t>() == 1;
    case LogicalTypeID::INT128: {
        auto v = value.getValue<int128_t>();
        return v.low == 1 && v.high == 0;
    }
    case LogicalTypeID::UINT128: {
        auto v = value.getValue<uint128_t>();
        return v.low == 1 && v.high == 0;
    }
    case LogicalTypeID::FLOAT:
        return value.getValue<float>() == 1.0f;
    case LogicalTypeID::DOUBLE:
        return value.getValue<double>() == 1.0;
    default:
        return false;
    }
}

std::shared_ptr<Expression> ExpressionBinder::bindExpression(
    const ParsedExpression& parsedExpression) {
    // Normally u can only reference an existing expression through alias which is a parsed
    // VARIABLE expression.
    // An exception is order by binding, e.g. RETURN a, COUNT(*) ORDER BY COUNT(*)
    // the later COUNT(*) should reference the one in projection list. So we need to explicitly
    // check scope when binding order by list.
    if (config.bindOrderByAfterAggregate && binder->scope.contains(parsedExpression.toString())) {
        return binder->scope.getExpression(parsedExpression.toString());
    }
    auto collector = ParsedParamExprCollector();
    collector.visit(&parsedExpression);
    if (collector.hasParamExprs()) {
        bool allParamExist = true;
        for (auto& parsedExpr : collector.getParamExprs()) {
            auto name = parsedExpr->constCast<ParsedParameterExpression>().getParameterName();
            if (!knownParameters.contains(name)) {
                unknownParameters.insert(name);
                allParamExist = false;
            }
        }
        if (!allParamExist) {
            auto expr = std::make_shared<ParameterExpression>(binder->getUniqueExpressionName(""),
                Value::createNullValue());
            if (parsedExpression.hasAlias()) {
                expr->setAlias(parsedExpression.getAlias());
            }
            return expr;
        }
    }
    std::shared_ptr<Expression> expression;
    auto expressionType = parsedExpression.getExpressionType();
    if (ExpressionTypeUtil::isBoolean(expressionType)) {
        expression = bindBooleanExpression(parsedExpression);
    } else if (ExpressionTypeUtil::isComparison(expressionType)) {
        expression = bindComparisonExpression(parsedExpression);
    } else if (ExpressionTypeUtil::isNullOperator(expressionType)) {
        expression = bindNullOperatorExpression(parsedExpression);
    } else if (ExpressionType::FUNCTION == expressionType) {
        expression = bindFunctionExpression(parsedExpression);
    } else if (ExpressionType::PROPERTY == expressionType) {
        expression = bindPropertyExpression(parsedExpression);
    } else if (ExpressionType::PARAMETER == expressionType) {
        expression = bindParameterExpression(parsedExpression);
    } else if (ExpressionType::LITERAL == expressionType) {
        expression = bindLiteralExpression(parsedExpression);
    } else if (ExpressionType::VARIABLE == expressionType) {
        expression = bindVariableExpression(parsedExpression);
    } else if (ExpressionType::SUBQUERY == expressionType) {
        expression = bindSubqueryExpression(parsedExpression);
    } else if (ExpressionType::CASE_ELSE == expressionType) {
        expression = bindCaseExpression(parsedExpression);
    } else if (ExpressionType::LAMBDA == expressionType) {
        expression = bindLambdaExpression(parsedExpression);
    } else {
        throw NotImplementedException(
            "bindExpression(" + ExpressionTypeUtil::toString(expressionType) + ").");
    }
    expression = simplifyExpression(expression);
    if (ConstantExpressionVisitor::needFold(*expression)) {
        return foldExpression(expression);
    }
    return expression;
}

std::shared_ptr<Expression> ExpressionBinder::simplifyExpression(
    const std::shared_ptr<Expression>& expression) {
    switch (expression->expressionType) {
    case ExpressionType::AND:
    case ExpressionType::OR:
    case ExpressionType::NOT:
        return simplifyBooleanExpression(expression);
    case ExpressionType::CASE_ELSE:
        return simplifyCaseExpression(expression);
    case ExpressionType::FUNCTION:
        return simplifyArithmeticExpression(expression);
    default:
        return expression;
    }
}

std::shared_ptr<Expression> ExpressionBinder::simplifyArithmeticExpression(
    const std::shared_ptr<Expression>& expression) {
    if (expression->getNumChildren() != 2) {
        return expression;
    }
    auto& funcExpr = expression->constCast<ScalarFunctionExpression>();
    const auto& name = funcExpr.getFunction().name;
    const bool isAdd = name == AddFunction::name;
    const bool isSubtract = name == SubtractFunction::name;
    const bool isMultiply = name == MultiplyFunction::name;
    const bool isDivide = name == DivideFunction::name;
    if (!isAdd && !isSubtract && !isMultiply && !isDivide) {
        return expression;
    }
    auto left = expression->getChild(0);
    auto right = expression->getChild(1);
    std::shared_ptr<Expression> survivor = nullptr;
    if (isAdd) {
        // x + 0 -> x, 0 + x -> x
        if (isIntegralZeroLiteral(*right)) {
            survivor = left;
        } else if (isIntegralZeroLiteral(*left)) {
            survivor = right;
        }
    } else if (isSubtract) {
        // x - 0 -> x (0 - x is negation, not an identity)
        if (isIntegralZeroLiteral(*right)) {
            survivor = left;
        }
    } else if (isMultiply) {
        // x * 1 -> x, 1 * x -> x (x * 0 -> 0 is NOT applied: NULL * 0 is NULL, not 0)
        if (isNumericOneLiteral(*right)) {
            survivor = left;
        } else if (isNumericOneLiteral(*left)) {
            survivor = right;
        }
    } else { // isDivide
        // x / 1 -> x (1 / x is a reciprocal, not an identity)
        if (isNumericOneLiteral(*right)) {
            survivor = left;
        }
    }
    if (survivor == nullptr) {
        return expression;
    }
    // The binder inserts implicit casts (e.g. INT col + DOUBLE 0.0 promotes to DOUBLE).
    // Only rewrite when the surviving operand already has the result type, so the
    // rewrite cannot change the expression's data type.
    if (expression->dataType != survivor->dataType) {
        return expression;
    }
    // Preserve the original expression name, mirroring foldExpression: without this,
    // e.g. RETURN t.age, t.age + 0 would produce two identically-named columns.
    if (expression->hasAlias()) {
        if (!survivor->hasAlias()) {
            survivor->setAlias(expression->getAlias());
        }
    } else if (!survivor->hasAlias()) {
        survivor->setAlias(expression->toString());
    }
    return survivor;
}

std::shared_ptr<Expression> ExpressionBinder::simplifyBooleanExpression(
    const std::shared_ptr<Expression>& expression) {
    auto expressionType = expression->expressionType;
    if (expressionType == ExpressionType::NOT) {
        auto child = simplifyExpression(expression->getChild(0));
        if (isBoolLiteral(*child, true)) {
            return createLiteralExpression(Value(false));
        }
        if (isBoolLiteral(*child, false)) {
            return createLiteralExpression(Value(true));
        }
        if (child->expressionType == ExpressionType::NOT) {
            return child->getChild(0);
        }
        if (child != expression->getChild(0)) {
            return bindBooleanExpression(expressionType, expression_vector{child});
        }
        return expression;
    }
    DASSERT(expressionType == ExpressionType::AND || expressionType == ExpressionType::OR);
    auto left = simplifyExpression(expression->getChild(0));
    auto right = simplifyExpression(expression->getChild(1));
    if (expressionType == ExpressionType::AND) {
        if (isBoolLiteral(*left, false)) {
            return left;
        }
        if (isBoolLiteral(*right, false)) {
            return right;
        }
        if (isBoolLiteral(*left, true)) {
            return right;
        }
        if (isBoolLiteral(*right, true)) {
            return left;
        }
    } else {
        if (isBoolLiteral(*left, true)) {
            return left;
        }
        if (isBoolLiteral(*right, true)) {
            return right;
        }
        if (isBoolLiteral(*left, false)) {
            return right;
        }
        if (isBoolLiteral(*right, false)) {
            return left;
        }
    }
    if (left != expression->getChild(0) || right != expression->getChild(1)) {
        return bindBooleanExpression(expressionType, expression_vector{left, right});
    }
    return expression;
}

std::shared_ptr<Expression> ExpressionBinder::simplifyCaseExpression(
    const std::shared_ptr<Expression>& expression) {
    auto& caseExpression = expression->constCast<CaseExpression>();
    auto elseExpression = simplifyExpression(caseExpression.getElseExpression());
    auto simplifiedCaseExpression = std::make_shared<CaseExpression>(expression->dataType.copy(),
        elseExpression, expression->getUniqueName());
    bool changed = elseExpression != caseExpression.getElseExpression();
    for (auto i = 0u; i < caseExpression.getNumCaseAlternatives(); ++i) {
        auto alternative = caseExpression.getCaseAlternative(i);
        auto whenExpression = simplifyExpression(alternative->whenExpression);
        auto thenExpression = simplifyExpression(alternative->thenExpression);
        if (ExpressionUtil::isNullLiteral(*whenExpression) ||
            isBoolLiteral(*whenExpression, false)) {
            changed = true;
            continue;
        }
        if (isBoolLiteral(*whenExpression, true)) {
            changed = true;
            if (simplifiedCaseExpression->getNumCaseAlternatives() == 0) {
                return thenExpression;
            }
            simplifiedCaseExpression->addCaseAlternative(whenExpression, thenExpression);
            break;
        }
        changed = changed || whenExpression != alternative->whenExpression ||
                  thenExpression != alternative->thenExpression;
        simplifiedCaseExpression->addCaseAlternative(whenExpression, thenExpression);
    }
    if (simplifiedCaseExpression->getNumCaseAlternatives() == 0) {
        return elseExpression;
    }
    return changed ? simplifiedCaseExpression : expression;
}

std::shared_ptr<Expression> ExpressionBinder::foldExpression(
    const std::shared_ptr<Expression>& expression) const {
    auto value =
        evaluator::ExpressionEvaluatorUtils::evaluateConstantExpression(expression, context);
    auto result = createLiteralExpression(value);
    // Fold result should preserve the alias original expression. E.g.
    // RETURN 2, 1 + 1 AS x
    // Once folded, 1 + 1 will become 2 and have the same identifier as the first RETURN element.
    // We preserve alias (x) to avoid such conflict.
    if (expression->hasAlias()) {
        result->setAlias(expression->getAlias());
    } else {
        result->setAlias(expression->toString());
    }
    return result;
}

static std::string unsupportedImplicitCastException(const Expression& expression,
    const std::string& targetTypeStr) {
    return std::format(
        "Expression {} has data type {} but expected {}. Implicit cast is not supported.",
        expression.toString(), expression.dataType.toString(), targetTypeStr);
}

std::shared_ptr<Expression> ExpressionBinder::implicitCastIfNecessary(
    const std::shared_ptr<Expression>& expression, const LogicalType& targetType) {
    auto& type = expression->dataType;
    if (type == targetType || targetType.containsAny()) { // No need to cast.
        return expression;
    }
    if (!type.isInternalType() || !targetType.isInternalType()) {
        return implicitCast(expression, targetType);
    }
    if (ExpressionUtil::canCastStatically(*expression, targetType)) {
        expression->cast(targetType);
        return expression;
    }
    return implicitCast(expression, targetType);
}

std::shared_ptr<Expression> ExpressionBinder::implicitCast(
    const std::shared_ptr<Expression>& expression, const LogicalType& targetType) {
    if (CastFunction::hasImplicitCast(expression->dataType, targetType)) {
        return forceCast(expression, targetType);
    } else {
        throw BinderException(unsupportedImplicitCastException(*expression, targetType.toString()));
    }
}

// cast without implicit checking.
std::shared_ptr<Expression> ExpressionBinder::forceCast(
    const std::shared_ptr<Expression>& expression, const LogicalType& targetType) {
    auto functionName = "CAST";
    auto children =
        expression_vector{expression, createLiteralExpression(Value(targetType.toString()))};
    auto castExpression = bindScalarFunctionExpression(children, functionName);
    if (expression->expressionType == ExpressionType::LITERAL &&
        expression->dataType == LogicalType::STRING() && LogicalTypeUtils::isDate(targetType)) {
        return foldExpression(castExpression);
    }
    return castExpression;
}

std::string ExpressionBinder::getUniqueName(const std::string& name) const {
    return binder->getUniqueExpressionName(name);
}

void ExpressionBinder::addParameter(const std::string& name, std::shared_ptr<Value> value) {
    DASSERT(!knownParameters.contains(name));
    knownParameters[name] = value;
}

} // namespace binder
} // namespace lbug

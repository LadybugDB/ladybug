#include <algorithm>

#include "binder/binder.h"
#include "binder/expression/case_expression.h"
#include "binder/expression/expression_util.h"
#include "binder/expression_binder.h"
#include "function/cast/vector_cast_functions.h"
#include "parser/expression/parsed_case_expression.h"

using namespace lbug::common;
using namespace lbug::parser;

namespace lbug {
namespace binder {

// Whether implicitCastIfNecessary would accept casting the expression to the type.
static bool canImplicitlyCast(const Expression& expression, const LogicalType& type) {
    return expression.dataType == type || type.containsAny() ||
           ExpressionUtil::canCastStatically(expression, type) ||
           function::CastFunction::hasImplicitCast(expression.dataType, type);
}

std::shared_ptr<Expression> ExpressionBinder::bindCaseExpression(
    const ParsedExpression& parsedExpression) {
    auto& parsedCaseExpression = parsedExpression.constCast<ParsedCaseExpression>();
    // The result type is the common type of every THEN and the ELSE, as for coalesce.
    expression_vector results;
    for (auto i = 0u; i < parsedCaseExpression.getNumCaseAlternative(); ++i) {
        results.push_back(
            bindExpression(*parsedCaseExpression.getCaseAlternative(i)->thenExpression));
    }
    if (parsedCaseExpression.hasElseExpression()) {
        results.push_back(bindExpression(*parsedCaseExpression.getElseExpression()));
    }
    auto resultType = LogicalType::ANY();
    if (!ExpressionUtil::tryCombineDataType(results, resultType) ||
        !std::all_of(results.begin(), results.end(),
            [&](const auto& result) { return canImplicitlyCast(*result, resultType); })) {
        // No common type: take the last typed THEN, else the ELSE, and cast the rest to it.
        resultType = LogicalType::ANY();
        for (auto i = 0u; i < parsedCaseExpression.getNumCaseAlternative(); ++i) {
            if (results[i]->getDataType().getLogicalTypeID() != LogicalTypeID::ANY) {
                resultType = results[i]->getDataType().copy();
            }
        }
        if (resultType.getLogicalTypeID() == LogicalTypeID::ANY &&
            parsedCaseExpression.hasElseExpression()) {
            resultType = results.back()->getDataType().copy();
        }
    }
    auto name = binder->getUniqueExpressionName(parsedExpression.getRawName());
    // bind ELSE ...
    std::shared_ptr<Expression> elseExpression;
    if (parsedCaseExpression.hasElseExpression()) {
        elseExpression = bindExpression(*parsedCaseExpression.getElseExpression());
    } else {
        elseExpression = createNullLiteralExpression();
    }
    elseExpression = implicitCastIfNecessary(elseExpression, resultType);
    auto boundCaseExpression =
        make_shared<CaseExpression>(resultType.copy(), std::move(elseExpression), name);
    // bind WHEN ... THEN ...
    if (parsedCaseExpression.hasCaseExpression()) {
        auto boundCase = bindExpression(*parsedCaseExpression.getCaseExpression());
        for (auto i = 0u; i < parsedCaseExpression.getNumCaseAlternative(); ++i) {
            auto caseAlternative = parsedCaseExpression.getCaseAlternative(i);
            auto boundWhen = bindExpression(*caseAlternative->whenExpression);
            boundWhen = implicitCastIfNecessary(boundWhen, boundCase->dataType);
            // rewrite "CASE a.age WHEN 1" as "CASE WHEN a.age = 1"
            if (ExpressionUtil::isNullLiteral(*boundWhen)) {
                boundWhen = bindNullOperatorExpression(ExpressionType::IS_NULL,
                    expression_vector{boundWhen});
            } else {
                boundWhen = bindComparisonExpression(ExpressionType::EQUALS,
                    expression_vector{boundCase, boundWhen});
            }
            auto boundThen = bindExpression(*caseAlternative->thenExpression);
            boundThen = implicitCastIfNecessary(boundThen, resultType);
            boundCaseExpression->addCaseAlternative(boundWhen, boundThen);
        }
    } else {
        for (auto i = 0u; i < parsedCaseExpression.getNumCaseAlternative(); ++i) {
            auto caseAlternative = parsedCaseExpression.getCaseAlternative(i);
            auto boundWhen = bindExpression(*caseAlternative->whenExpression);
            boundWhen = implicitCastIfNecessary(boundWhen, LogicalType::BOOL());
            auto boundThen = bindExpression(*caseAlternative->thenExpression);
            boundThen = implicitCastIfNecessary(boundThen, resultType);
            boundCaseExpression->addCaseAlternative(boundWhen, boundThen);
        }
    }
    return boundCaseExpression;
}

} // namespace binder
} // namespace lbug

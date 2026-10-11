#include "expression_evaluator/expression_evaluator_utils.h"

#include "binder/expression/case_expression.h"
#include "binder/expression/literal_expression.h"
#include "common/types/value/nested.h"
#include "common/types/value/value.h"
#include "processor/expression_mapper.h"

using namespace lbug::common;
using namespace lbug::processor;

namespace lbug {
namespace evaluator {

static uint64_t getNumNestedValues(const ValueVector& vector) {
    switch (vector.dataType.getPhysicalType()) {
    case PhysicalTypeID::LIST:
    case PhysicalTypeID::ARRAY:
        return ListVector::getDataVectorSize(&vector) +
               getNumNestedValues(*ListVector::getDataVector(&vector));
    case PhysicalTypeID::STRUCT: {
        uint64_t numValues = 0;
        for (const auto& fieldVector : StructVector::getFieldVectors(&vector)) {
            numValues += getNumNestedValues(*fieldVector);
        }
        return numValues;
    }
    default:
        return 0;
    }
}

static uint64_t getNumNestedValues(const Value& value) {
    uint64_t numValues = value.getChildrenSize();
    for (auto i = 0u; i < value.getChildrenSize(); i++) {
        numValues += getNumNestedValues(*NestedVal::getChildVal(&value, i));
    }
    return numValues;
}

// Sub-expressions plus the nested values of the literals among them.
static uint64_t getExpressionSize(const binder::Expression& expression) {
    uint64_t size = 1;
    if (expression.expressionType == ExpressionType::LITERAL) {
        size += getNumNestedValues(expression.constCast<binder::LiteralExpression>().value);
    } else if (expression.expressionType == ExpressionType::CASE_ELSE) {
        // CASE keeps its branches outside the children.
        const auto& caseExpression = expression.constCast<binder::CaseExpression>();
        for (auto i = 0u; i < caseExpression.getNumCaseAlternatives(); i++) {
            const auto* alternative = caseExpression.getCaseAlternative(i);
            size += getExpressionSize(*alternative->whenExpression) +
                    getExpressionSize(*alternative->thenExpression);
        }
        size += getExpressionSize(*caseExpression.getElseExpression());
    }
    for (auto i = 0u; i < expression.getNumChildren(); i++) {
        size += getExpressionSize(*expression.getChild(i));
    }
    return size;
}

static std::unique_ptr<ExpressionEvaluator> evaluateConstant(
    const std::shared_ptr<binder::Expression>& expression, main::ClientContext* clientContext,
    ResultSet& emptyResultSet) {
    auto evaluator = ExpressionMapper().getConstantEvaluator(expression);
    evaluator->init(emptyResultSet, clientContext);
    evaluator->evaluate();
    return evaluator;
}

static Value getConstantValue(const ExpressionEvaluator& evaluator) {
    auto& selVector = evaluator.resultVector->state->getSelVector();
    DASSERT(selVector.getSelSize() == 1);
    return *evaluator.resultVector->getAsValue(selVector[0]);
}

std::optional<Value> ExpressionEvaluatorUtils::tryEvaluateConstantExpression(
    std::shared_ptr<binder::Expression> expression, main::ClientContext* clientContext) {
    ResultSet emptyResultSet{0};
    auto evaluator = evaluateConstant(expression, clientContext, emptyResultSet);
    const auto numValues = getNumNestedValues(*evaluator->resultVector);
    if (numValues > MAX_FOLDED_NESTED_VALUES && numValues > getExpressionSize(*expression)) {
        return std::nullopt;
    }
    return getConstantValue(*evaluator);
}

Value ExpressionEvaluatorUtils::evaluateConstantExpression(
    std::shared_ptr<binder::Expression> expression, main::ClientContext* clientContext) {
    ResultSet emptyResultSet{0};
    return getConstantValue(*evaluateConstant(expression, clientContext, emptyResultSet));
}

} // namespace evaluator
} // namespace lbug

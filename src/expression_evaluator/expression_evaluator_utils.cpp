#include "expression_evaluator/expression_evaluator_utils.h"

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
    if (getNumNestedValues(*evaluator->resultVector) > MAX_FOLDED_NESTED_VALUES) {
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

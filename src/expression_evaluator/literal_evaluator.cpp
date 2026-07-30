#include "expression_evaluator/literal_evaluator.h"

#include "binder/expression/parameter_expression.h"
#include "common/types/value/value.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::main;

namespace lbug {
namespace evaluator {

void LiteralExpressionEvaluator::evaluate() {}

void LiteralExpressionEvaluator::evaluate(sel_t count) {
    unFlatState->getSelVectorUnsafe().setSelSize(count);
    resultVector->setState(unFlatState);
    for (auto i = 1ul; i < count; i++) {
        resultVector->copyFromVectorData(i, resultVector.get(), 0);
    }
}

bool LiteralExpressionEvaluator::selectInternal(SelectionVector&) {
    DASSERT(resultVector->dataType.getLogicalTypeID() == LogicalTypeID::BOOL);
    auto pos = resultVector->state->getSelVector()[0];
    DASSERT(pos == 0u);
    return resultVector->getValue<bool>(pos) && (!resultVector->isNull(pos));
}

void LiteralExpressionEvaluator::resolveResultVector(const processor::ResultSet& /*resultSet*/,
    MemoryManager* memoryManager) {
    resultVector = std::make_shared<ValueVector>(value.getDataType().copy(), memoryManager);
    flatState = DataChunkState::getSingleValueDataChunkState();
    unFlatState = std::make_shared<DataChunkState>();
    resultVector->setState(flatState);
    // If the expression is a bound parameter, read the fresh value from the expression
    // rather than the frozen copy captured at plan-build time. This enables the cached
    // physical-plan path to pick up changed parameter values between executions.
    const auto& srcValue = expression->expressionType == common::ExpressionType::PARAMETER ?
                               expression->constCast<binder::ParameterExpression>().getValue() :
                               value;
    if (srcValue.isNull()) {
        resultVector->setNull(0 /* pos */, true);
    } else {
        resultVector->copyFromValue(resultVector->state->getSelVector()[0], srcValue);
    }
}

} // namespace evaluator
} // namespace lbug

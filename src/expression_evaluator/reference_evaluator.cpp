#include "expression_evaluator/reference_evaluator.h"

using namespace lbug::common;
using namespace lbug::main;

namespace lbug {
namespace evaluator {

inline static bool isTrue(ValueVector& vector, uint64_t pos) {
    DASSERT(vector.dataType.getLogicalTypeID() == LogicalTypeID::BOOL);
    return !vector.isNull(pos) && vector.getValue<bool>(pos);
}

bool ReferenceExpressionEvaluator::selectInternal(SelectionVector& selVector) {
    uint64_t numSelectedValues = 0;
    auto selectedBuffer = selVector.getMutableBuffer();
    auto& resultSelVector = resultVector->state->getSelVector();
    if (resultSelVector.isUnfiltered()) {
        for (auto i = 0u; i < resultSelVector.getSelSize(); i++) {
            selectedBuffer[numSelectedValues] = i;
            numSelectedValues += isTrue(*resultVector, i);
        }
    } else {
        for (auto i = 0u; i < resultSelVector.getSelSize(); i++) {
            auto pos = resultSelVector[i];
            selectedBuffer[numSelectedValues] = pos;
            numSelectedValues += isTrue(*resultVector, pos);
        }
    }
    selVector.setSelSize(numSelectedValues);
    return numSelectedValues > 0;
}

} // namespace evaluator
} // namespace lbug

#include "expression_evaluator/internal_id_evaluator.h"

using namespace lbug::common;
using namespace lbug::main;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace evaluator {

void InternalIDExpressionEvaluator::resolveResultVector(const ResultSet& /*resultSet*/,
    MemoryManager* memoryManager) {
    resultVector =
        std::make_shared<ValueVector>(LogicalType(LogicalTypeID::INTERNAL_ID), memoryManager);
    std::vector<ExpressionEvaluator*> inputEvaluators{children[0].get()};
    resolveResultStateFromChildren(inputEvaluators);
}

void InternalIDExpressionEvaluator::evaluate() {
    for (auto& child : children) {
        child->evaluate();
    }
    auto& baseVector = children[0]->resultVector;
    auto& selVector = baseVector->state->getSelVector();
    for (auto i = 0u; i < selVector.getSelSize(); ++i) {
        auto pos = selVector[i];
        resultVector->setValue<nodeID_t>(pos, baseVector->getValue<nodeID_t>(pos));
        resultVector->setNull(pos, baseVector->isNull(pos));
    }
}

bool InternalIDExpressionEvaluator::selectInternal(SelectionVector& selVector) {
    evaluate();
    return updateSelectedPos(selVector);
}

} // namespace evaluator
} // namespace lbug

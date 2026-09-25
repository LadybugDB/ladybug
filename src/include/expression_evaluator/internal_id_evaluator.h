#pragma once

#include "expression_evaluator/expression_evaluator.h"

namespace lbug {
namespace evaluator {

// Evaluates an internal-ID property straight from its bound base node/relationship value
// (IDs are stored inline in node/rel values, so this is a copy, not a storage read).
// Used when no ID vector was materialized for the base (e.g. nodes produced by UNWIND);
// other properties still require scan-sourced vectors. Shares the evaluator type with
// reference evaluation (no visitor special-casing needed).
class InternalIDExpressionEvaluator : public ExpressionEvaluator {
    static constexpr EvaluatorType type_ = EvaluatorType::REFERENCE;

public:
    InternalIDExpressionEvaluator(std::shared_ptr<binder::Expression> expression,
        std::unique_ptr<ExpressionEvaluator> baseEvaluator)
        : ExpressionEvaluator{type_, std::move(expression)} {
        children.push_back(std::move(baseEvaluator));
    }

    void evaluate() override;
    // Selection is only meaningful for boolean expressions (the binder rejects anything
    // else in filter positions), so reaching here indicates misuse.
    bool selectInternal(common::SelectionVector& selVector) override;

    std::unique_ptr<ExpressionEvaluator> copy() override {
        return std::make_unique<InternalIDExpressionEvaluator>(expression, children[0]->copy());
    }

protected:
    void resolveResultVector(const processor::ResultSet& resultSet,
        storage::MemoryManager* memoryManager) override;
};

} // namespace evaluator
} // namespace lbug

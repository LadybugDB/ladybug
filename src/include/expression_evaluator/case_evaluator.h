#pragma once

#include <bitset>

#include "binder/expression/expression.h"
#include "common/system_config.h"
#include "expression_evaluator.h"

namespace lbug {
namespace main {
class ClientContext;
}

namespace evaluator {

struct CaseAlternativeEvaluator {
    std::unique_ptr<ExpressionEvaluator> whenEvaluator;
    std::unique_ptr<ExpressionEvaluator> thenEvaluator;
    std::unique_ptr<common::SelectionVector> whenSelVector;

    CaseAlternativeEvaluator(std::unique_ptr<ExpressionEvaluator> whenEvaluator,
        std::unique_ptr<ExpressionEvaluator> thenEvaluator)
        : whenEvaluator{std::move(whenEvaluator)}, thenEvaluator{std::move(thenEvaluator)} {}
    EXPLICIT_COPY_DEFAULT_MOVE(CaseAlternativeEvaluator);

    void init(const processor::ResultSet& resultSet, main::ClientContext* clientContext);

private:
    CaseAlternativeEvaluator(const CaseAlternativeEvaluator& other)
        : whenEvaluator{other.whenEvaluator->copy()}, thenEvaluator{other.thenEvaluator->copy()} {}
};

class CaseExpressionEvaluator : public ExpressionEvaluator {
    static constexpr EvaluatorType type_ = EvaluatorType::CASE_ELSE;

public:
    CaseExpressionEvaluator(std::shared_ptr<binder::Expression> expression,
        std::vector<CaseAlternativeEvaluator> alternativeEvaluators,
        std::unique_ptr<ExpressionEvaluator> elseEvaluator)
        : ExpressionEvaluator{type_, std::move(expression)},
          alternativeEvaluators{std::move(alternativeEvaluators)},
          elseEvaluator{std::move(elseEvaluator)} {}

    const std::vector<CaseAlternativeEvaluator>& getAlternativeEvaluators() const {
        return alternativeEvaluators;
    }
    ExpressionEvaluator* getElseEvaluator() const { return elseEvaluator.get(); }

    void init(const processor::ResultSet& resultSet, main::ClientContext* clientContext) override;

    void evaluate() override;

    bool selectInternal(common::SelectionVector& selVector) override;

    std::unique_ptr<ExpressionEvaluator> copy() override {
        return std::make_unique<CaseExpressionEvaluator>(expression,
            copyVector(alternativeEvaluators), elseEvaluator->copy());
    }

protected:
    void resolveResultVector(const processor::ResultSet& resultSet,
        storage::MemoryManager* memoryManager) override;

private:
    void fillSelected(const common::SelectionVector& selVector, common::ValueVector* srcVector);

    void fillAll(common::ValueVector* srcVector);

    void fillEntry(common::sel_t resultPos, common::ValueVector* srcVector);

private:
    void collectUnfilledMask();

private:
    std::vector<CaseAlternativeEvaluator> alternativeEvaluators;
    std::unique_ptr<ExpressionEvaluator> elseEvaluator;

    std::bitset<common::DEFAULT_VECTOR_CAPACITY> filledMask;
    // Scratch buffer holding the row positions a branch must be evaluated on. Each branch is
    // evaluated only on the rows that need it, so errors on rows that take another branch
    // (e.g. division by zero) cannot fail the query.
    std::unique_ptr<common::SelectionVector> maskedSelVector =
        std::make_unique<common::SelectionVector>(common::DEFAULT_VECTOR_CAPACITY);
    // Scratch storage for collecting the data chunk states of a branch. One entry per distinct
    // state; reused across batches (each CASE evaluator instance is only used by one thread
    // at a time, like the other mutable evaluation state above).
    std::vector<common::DataChunkState*> branchStatesScratch;
};

} // namespace evaluator
} // namespace lbug

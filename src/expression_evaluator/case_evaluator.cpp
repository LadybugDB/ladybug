#include "expression_evaluator/case_evaluator.h"

#include <algorithm>
#include <cstring>
#include <vector>

using namespace lbug::main;
using namespace lbug::common;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace evaluator {

namespace {

// Collect the distinct row-indexed data chunk states that feed a branch evaluator, so the
// branch can be evaluated on a narrowed selection of rows. Recurses into `children` and,
// for nested CASE evaluators, into their WHEN/THEN/ELSE evaluators (kept outside `children`).
// Element-indexed evaluators (e.g. list-lambda roots, which run over list elements rather
// than batch rows) are deliberately not descended into: narrowing their vectors with row
// positions would corrupt them.
void collectBranchStates(ExpressionEvaluator* evaluator, std::vector<DataChunkState*>& states) {
    if (evaluator == nullptr || evaluator->resultVector == nullptr ||
        evaluator->resultVector->state == nullptr) {
        return;
    }
    auto* state = evaluator->resultVector->state.get();
    if (std::find(states.begin(), states.end(), state) == states.end()) {
        states.push_back(state);
    }
    if (evaluator->getEvaluatorType() == EvaluatorType::CASE_ELSE) {
        auto& caseEvaluator = evaluator->cast<CaseExpressionEvaluator>();
        for (auto& alternative : caseEvaluator.getAlternativeEvaluators()) {
            collectBranchStates(alternative.whenEvaluator.get(), states);
            collectBranchStates(alternative.thenEvaluator.get(), states);
        }
        collectBranchStates(caseEvaluator.getElseEvaluator(), states);
    } else {
        for (auto& child : evaluator->getChildren()) {
            collectBranchStates(child.get(), states);
        }
    }
}

// Narrows every given row-indexed state to `mask` for the lifetime of the guard and restores
// the original selections on destruction (including when the guarded evaluation throws).
class ScopedBranchSelection {
public:
    ScopedBranchSelection(const std::vector<DataChunkState*>& states, const SelectionVector& mask) {
        const auto maskSize = mask.getSelSize();
        if (maskSize == 0) {
            return;
        }
        const auto maskPositions = mask.getSelectedPositions();
        for (auto* state : states) {
            if (state == nullptr || state->isFlat()) {
                continue;
            }
            auto& selVector = state->getSelVectorUnsafe();
            SavedSelection saved;
            saved.state = state;
            saved.selSize = selVector.getSelSize();
            saved.unfiltered = selVector.isUnfiltered();
            saved.positions.assign(selVector.getSelectedPositions().begin(),
                selVector.getSelectedPositions().end());
            selVector.setToFiltered();
            std::memcpy(selVector.getMutableBuffer().data(), maskPositions.data(),
                maskSize * sizeof(sel_t));
            selVector.setSelSize(maskSize);
            savedSelections.push_back(std::move(saved));
        }
    }

    ~ScopedBranchSelection() {
        for (auto& saved : savedSelections) {
            auto& selVector = saved.state->getSelVectorUnsafe();
            if (saved.unfiltered) {
                selVector.setToUnfiltered(saved.selSize);
            } else {
                selVector.setToFiltered();
                std::memcpy(selVector.getMutableBuffer().data(), saved.positions.data(),
                    saved.selSize * sizeof(sel_t));
                selVector.setSelSize(saved.selSize);
            }
        }
    }

    DELETE_COPY_AND_MOVE(ScopedBranchSelection);

private:
    struct SavedSelection {
        DataChunkState* state = nullptr;
        sel_t selSize = 0;
        bool unfiltered = false;
        std::vector<sel_t> positions;
    };

    std::vector<SavedSelection> savedSelections;
};

} // namespace

void CaseAlternativeEvaluator::init(const ResultSet& resultSet,
    main::ClientContext* clientContext) {
    whenEvaluator->init(resultSet, clientContext);
    thenEvaluator->init(resultSet, clientContext);
    whenSelVector = std::make_unique<SelectionVector>(DEFAULT_VECTOR_CAPACITY);
    whenSelVector->setToFiltered();
}

void CaseExpressionEvaluator::init(const ResultSet& resultSet, main::ClientContext* clientContext) {
    for (auto& alternativeEvaluator : alternativeEvaluators) {
        alternativeEvaluator.init(resultSet, clientContext);
    }
    elseEvaluator->init(resultSet, clientContext);
    ExpressionEvaluator::init(resultSet, clientContext);
}

void CaseExpressionEvaluator::collectUnfilledMask() {
    maskedSelVector->setToFiltered();
    auto buffer = maskedSelVector->getMutableBuffer();
    const auto& selVector = resultVector->state->getSelVector();
    sel_t count = 0;
    for (auto i = 0u; i < selVector.getSelSize(); ++i) {
        const auto pos = selVector[i];
        if (!filledMask[pos]) {
            buffer[count++] = pos;
        }
    }
    maskedSelVector->setSelSize(count);
}

void CaseExpressionEvaluator::evaluate() {
    filledMask.reset();
    const auto fullSelSize = resultVector->state->getSelVector().getSelSize();
    for (auto& alternativeEvaluator : alternativeEvaluators) {
        auto* whenSelVector = alternativeEvaluator.whenSelVector.get();
        const bool isFlatWhen = alternativeEvaluator.whenEvaluator->isResultFlat();
        // Narrow the WHEN evaluation to rows that earlier alternatives have not filled yet, so
        // an error on an already-decided row cannot fail the query.
        bool hasAtLeastOneValue = false;
        if (!isFlatWhen && filledMask.count() != 0) {
            collectUnfilledMask();
            branchStatesScratch.clear();
            collectBranchStates(alternativeEvaluator.whenEvaluator.get(), branchStatesScratch);
            ScopedBranchSelection narrowWhen(branchStatesScratch, *maskedSelVector);
            hasAtLeastOneValue = alternativeEvaluator.whenEvaluator->select(*whenSelVector, false);
        } else {
            // the sel vector is already set to filtered in init()
            hasAtLeastOneValue = alternativeEvaluator.whenEvaluator->select(*whenSelVector, false);
        }
        if (!hasAtLeastOneValue) {
            continue;
        }
        // Rows taking this branch: with a flat (constant-true) WHEN every unfilled row does;
        // with an unflat WHEN only the selected rows do (already limited to unfilled rows when
        // the WHEN evaluation above was narrowed, and no row is filled yet otherwise).
        const SelectionVector* branchMask = whenSelVector;
        if (isFlatWhen) {
            collectUnfilledMask();
            branchMask = maskedSelVector.get();
        }
        // Evaluate THEN only on rows taking this branch, so an error on a row that falls
        // through to a later branch (e.g. division by zero) is never raised.
        auto* thenEvaluator = alternativeEvaluator.thenEvaluator.get();
        if (!thenEvaluator->isResultFlat() && branchMask->getSelSize() != fullSelSize) {
            branchStatesScratch.clear();
            collectBranchStates(thenEvaluator, branchStatesScratch);
            ScopedBranchSelection narrowThen(branchStatesScratch, *branchMask);
            thenEvaluator->evaluate();
        } else {
            thenEvaluator->evaluate();
        }
        fillSelected(*branchMask, thenEvaluator->resultVector.get());
        if (filledMask.count() == fullSelSize) {
            return;
        }
    }
    collectUnfilledMask();
    if (maskedSelVector->getSelSize() == 0) {
        return;
    }
    // Evaluate ELSE only on rows that no WHEN matched.
    if (!elseEvaluator->isResultFlat() && maskedSelVector->getSelSize() != fullSelSize) {
        branchStatesScratch.clear();
        collectBranchStates(elseEvaluator.get(), branchStatesScratch);
        ScopedBranchSelection narrowElse(branchStatesScratch, *maskedSelVector);
        elseEvaluator->evaluate();
    } else {
        elseEvaluator->evaluate();
    }
    fillSelected(*maskedSelVector, elseEvaluator->resultVector.get());
}

bool CaseExpressionEvaluator::selectInternal(SelectionVector& selVector) {
    evaluate();
    DASSERT(resultVector->state->getSelVector().getSelSize() != 0);
    DASSERT(selVector.getSelSize() != 0);
    DASSERT(resultVector->state->getSelVector().getSelSize() == selVector.getSelSize());
    auto numSelectedValues = 0u;
    auto selectedPosBuffer = selVector.getMutableBuffer();
    for (auto i = 0u; i < selVector.getSelSize(); ++i) {
        auto selVectorPos = selVector[i];
        auto resultVectorPos = resultVector->state->getSelVector()[i];
        selectedPosBuffer[numSelectedValues] = selVectorPos;
        const bool selectCurrentValue =
            !resultVector->isNull(resultVectorPos) && resultVector->getValue<bool>(resultVectorPos);
        numSelectedValues += selectCurrentValue;
    }
    selVector.setSelSize(numSelectedValues);
    return numSelectedValues > 0;
}

void CaseExpressionEvaluator::resolveResultVector(const ResultSet& /*resultSet*/,
    MemoryManager* memoryManager) {
    resultVector = std::make_shared<ValueVector>(expression->dataType.copy(), memoryManager);
    std::vector<ExpressionEvaluator*> inputEvaluators;
    for (auto& alternative : alternativeEvaluators) {
        inputEvaluators.push_back(alternative.whenEvaluator.get());
        inputEvaluators.push_back(alternative.thenEvaluator.get());
    }
    inputEvaluators.push_back(elseEvaluator.get());
    resolveResultStateFromChildren(inputEvaluators);
}

void CaseExpressionEvaluator::fillSelected(const SelectionVector& selVector,
    ValueVector* srcVector) {
    for (auto i = 0u; i < selVector.getSelSize(); ++i) {
        auto resultPos = selVector[i];
        fillEntry(resultPos, srcVector);
    }
}

void CaseExpressionEvaluator::fillAll(ValueVector* srcVector) {
    auto& resultSelVector = resultVector->state->getSelVector();
    for (auto i = 0u; i < resultSelVector.getSelSize(); ++i) {
        auto resultPos = resultSelVector[i];
        fillEntry(resultPos, srcVector);
    }
}

void CaseExpressionEvaluator::fillEntry(sel_t resultPos, ValueVector* srcVector) {
    if (filledMask[resultPos]) {
        return;
    }
    filledMask[resultPos] = true;
    auto srcPos = srcVector->state->isFlat() ? srcVector->state->getSelVector()[0] : resultPos;
    resultVector->copyFromVectorData(resultPos, srcVector, srcPos);
}

} // namespace evaluator
} // namespace lbug

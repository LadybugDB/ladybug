#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "expression_evaluator/lambda_evaluator.h"
#include "expression_evaluator/list_slice_info.h"
#include "function/function.h"
#include "function/list/vector_list_functions.h"

using namespace lbug::common;

namespace lbug {
namespace function {

void execQuantifierFunc(quantifier_handler handler,
    const std::vector<std::shared_ptr<common::ValueVector>>& input,
    const std::vector<common::SelectionVector*>& inputSelVectors, common::ValueVector& result,
    common::SelectionVector* resultSelVector, void* bindData) {
    auto listLambdaBindData = reinterpret_cast<evaluator::ListLambdaBindData*>(bindData);
    auto* sliceInfo = listLambdaBindData->sliceInfo;
    auto& inputVector = *input[0];
    DASSERT(input.size() == 2);

    auto savedParamStates =
        sliceInfo->overrideAndSaveParamStates(listLambdaBindData->lambdaParamEvaluators);

    listLambdaBindData->rootEvaluator->evaluate();

    auto& filterVector = *input[1];
    auto& filterSelVector = *inputSelVectors[1];
    DASSERT(filterSelVector.isUnfiltered());
    bool isConstantTrueExpr = listLambdaBindData->lambdaParamEvaluators.empty() &&
                              !filterVector.isNull(filterSelVector[0]) &&
                              filterVector.getValue<bool>(filterSelVector[0]);

    if (!isConstantTrueExpr && !listLambdaBindData->lambdaParamEvaluators.empty()) {
        // Accumulate per-list counts for this slice. The filter vector is indexed by
        // slice position (unfiltered, size == slice size), not by data offset.
        for (sel_t i = 0; i < sliceInfo->getSliceSize(); ++i) {
            if (!filterVector.isNull(i) && filterVector.getValue<bool>(i)) {
                const auto [listEntryPos, dataOffset] = sliceInfo->getPos(i);
                (void)dataOffset;
                sliceInfo->incrementQuantifierCount(listEntryPos);
            }
        }
    }

    if (sliceInfo->done()) {
        auto& listInputSelVector = *inputSelVectors[0];
        for (auto i = 0u; i < listInputSelVector.getSelSize(); ++i) {
            auto pos = listInputSelVector[i];
            if (inputVector.isNull(pos)) {
                result.setNull((*resultSelVector)[i], true);
                continue;
            }
            result.setNull((*resultSelVector)[i], false);
            auto srcListEntry = inputVector.getValue<list_entry_t>(pos);
            uint64_t numSelectedValues = 0;
            if (isConstantTrueExpr) {
                numSelectedValues = srcListEntry.size;
            } else if (listLambdaBindData->lambdaParamEvaluators.empty()) {
                // Constant false (or null, treated as false here since isConstantTrue
                // requires non-null true).
                numSelectedValues = 0;
            } else {
                numSelectedValues = sliceInfo->getQuantifierCount(pos);
            }
            result.setValue((*resultSelVector)[i], handler(numSelectedValues, srcListEntry.size));
        }
    }

    sliceInfo->restoreParamStates(listLambdaBindData->lambdaParamEvaluators,
        std::move(savedParamStates));
}

std::unique_ptr<FunctionBindData> bindQuantifierFunc(const ScalarBindFuncInput& input) {
    std::vector<common::LogicalType> paramTypes;
    paramTypes.push_back(input.arguments[0]->getDataType().copy());
    paramTypes.push_back(input.arguments[1]->getDataType().copy());
    return std::make_unique<FunctionBindData>(std::move(paramTypes), common::LogicalType::BOOL());
}

} // namespace function
} // namespace lbug

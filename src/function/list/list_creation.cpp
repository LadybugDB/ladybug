#include <unordered_set>

#include "binder/expression/expression_util.h"
#include "function/list/vector_list_functions.h"
#include "function/scalar_function.h"

using namespace lbug::common;

namespace lbug {
namespace function {

void ListCreationFunction::execFunc(
    const std::vector<std::shared_ptr<common::ValueVector>>& parameters,
    const std::vector<common::SelectionVector*>& parameterSelVectors, common::ValueVector& result,
    common::SelectionVector* resultSelVector, void* /*dataPtr*/) {
    result.resetAuxiliaryBuffer();
    for (auto selectedPos = 0u; selectedPos < resultSelVector->getSelSize(); ++selectedPos) {
        auto pos = (*resultSelVector)[selectedPos];
        auto resultEntry = ListVector::addList(&result, parameters.size());
        result.setValue(pos, resultEntry);
        auto resultDataVector = ListVector::getDataVector(&result);
        auto resultPos = resultEntry.offset;
        for (auto i = 0u; i < parameters.size(); i++) {
            const auto& parameter = parameters[i];
            const auto& parameterSelVector = *parameterSelVectors[i];
            auto paramPos = parameter->state->isFlat() ? parameterSelVector[0] : pos;
            resultDataVector->copyFromVectorData(resultPos++, parameter.get(), paramPos);
        }
    }
}

static std::unique_ptr<FunctionBindData> bindFunc(const ScalarBindFuncInput& input) {
    LogicalType combinedType(LogicalTypeID::ANY);
    std::unordered_set<LogicalTypeID> distinctTypes;
    for (auto& arg : input.arguments) {
        auto typeID = arg->getDataType().getLogicalTypeID();
        if (typeID != LogicalTypeID::ANY) {
            distinctTypes.insert(typeID);
        }
    }
    const bool mixedConcreteTypes = distinctTypes.size() > 1;
    if (mixedConcreteTypes) {
        // First, let the type system try to find a common super type (e.g. numeric promotion).
        binder::ExpressionUtil::tryCombineDataType(input.arguments, combinedType);
        if (combinedType.getLogicalTypeID() == LogicalTypeID::ANY) {
            // No common type found. For UNWIND-style mixed lists, if there is any STRING element
            // we upcast the whole list to STRING so queries like
            //   UNWIND [1, 'hello', true, 3.14, null] AS x RETURN x
            // work as expected. Otherwise fall back to the first concrete element type so that
            // expressions like [123, True] trigger the implicit cast error on True (BOOL->INT64),
            // matching the TCK Map1.Scenario6 expectation.
            if (distinctTypes.contains(LogicalTypeID::STRING)) {
                combinedType = LogicalType::STRING();
            } else {
                for (auto& arg : input.arguments) {
                    if (arg->getDataType().getLogicalTypeID() != LogicalTypeID::ANY) {
                        combinedType = arg->getDataType().copy();
                        break;
                    }
                }
            }
        }
    } else {
        binder::ExpressionUtil::tryCombineDataType(input.arguments, combinedType);
        if (combinedType.getLogicalTypeID() == LogicalTypeID::ANY) {
            combinedType = LogicalType::INT64();
        }
    }
    auto resultType = LogicalType::LIST(combinedType.copy());
    auto bindData = std::make_unique<FunctionBindData>(std::move(resultType));
    for (auto& _ : input.arguments) {
        (void)_;
        bindData->paramTypes.push_back(combinedType.copy());
    }
    return bindData;
}

function_set ListCreationFunction::getFunctionSet() {
    function_set result;
    auto function = std::make_unique<ScalarFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::ANY}, LogicalTypeID::LIST, execFunc);
    function->bindFunc = bindFunc;
    function->isVarLength = true;
    result.push_back(std::move(function));
    return result;
}

} // namespace function
} // namespace lbug

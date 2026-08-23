#include "binder/expression/literal_expression.h"
#include "common/constants.h"
#include "common/exception/binder.h"
#include "common/vector/value_vector.h"
#include "function/path/vector_path_functions.h"
#include "function/scalar_function.h"
#include <format>

using namespace lbug::common;
using namespace lbug::binder;

namespace lbug {
namespace function {

static bool isInternalField(const std::string& field) {
    return field == InternalKeyword::ID || field == InternalKeyword::LABEL ||
           field == InternalKeyword::SRC || field == InternalKeyword::DST ||
           field == InternalKeyword::DIRECTION || field == InternalKeyword::LENGTH ||
           field == InternalKeyword::NODES || field == InternalKeyword::RELS ||
           field == InternalKeyword::PLACE_HOLDER;
}

static std::unique_ptr<FunctionBindData> bindFunc(const ScalarBindFuncInput& input) {
    if (input.arguments[1]->expressionType != ExpressionType::LITERAL) {
        throw BinderException(std::format("Expected literal input as the second argument for {}().",
            PropertiesFunction::name));
    }
    auto literalExpr = input.arguments[1]->constPtrCast<LiteralExpression>();
    auto key = literalExpr->getValue().getValue<std::string>();
    const auto& listType = input.arguments[0]->getDataType();
    const auto& childType = ListType::getChildType(listType);
    struct_field_idx_t fieldIdx = 0;
    if (childType.getLogicalTypeID() == LogicalTypeID::NODE ||
        childType.getLogicalTypeID() == LogicalTypeID::REL) {
        fieldIdx = StructType::getFieldIdx(childType, key);
        if (fieldIdx == INVALID_STRUCT_FIELD_IDX) {
            throw BinderException(std::format("Invalid property name: {}.", key));
        }
    } else {
        throw BinderException(
            std::format("Cannot extract properties from {}.", listType.toString()));
    }
    const auto& field = StructType::getField(childType, fieldIdx);
    auto returnType = LogicalType::LIST(field.getType().copy());
    auto bindData = std::make_unique<PropertiesBindData>(std::move(returnType), fieldIdx);
    bindData->paramTypes.push_back(input.arguments[0]->getDataType().copy());
    bindData->paramTypes.push_back(LogicalType(input.definition->parameterTypeIDs[1]));
    return bindData;
}

static void compileFunc(FunctionBindData* bindData,
    const std::vector<std::shared_ptr<ValueVector>>& parameters,
    std::shared_ptr<ValueVector>& result) {
    DASSERT(parameters[0]->dataType.getPhysicalType() == PhysicalTypeID::LIST);
    auto& propertiesBindData = bindData->cast<PropertiesBindData>();
    auto fieldVector = StructVector::getFieldVector(ListVector::getDataVector(parameters[0].get()),
        propertiesBindData.childIdx);
    ListVector::setDataVector(result.get(), fieldVector);
}

static void execFunc(const std::vector<std::shared_ptr<common::ValueVector>>& parameters,
    const std::vector<common::SelectionVector*>& parameterSelVectors, common::ValueVector& result,
    common::SelectionVector* resultSelVector, void* /*dataPtr*/) {
    ListVector::copyListEntryAndBufferMetaData(result, *resultSelVector, *parameters[0],
        *parameterSelVectors[0]);
}

static std::unique_ptr<FunctionBindData> singleArgBindFunc(const ScalarBindFuncInput& input) {
    const auto& structType = input.arguments[0]->getDataType();
    auto numFields = StructType::getNumFields(structType);
    std::vector<StructField> resultFields;
    std::vector<struct_field_idx_t> fieldIndices;
    for (auto i = 0u; i < numFields; ++i) {
        const auto& field = StructType::getField(structType, i);
        if (isInternalField(field.getName())) {
            continue;
        }
        resultFields.emplace_back(field.getName(), field.getType().copy());
        fieldIndices.push_back(i);
    }
    auto returnType = LogicalType::STRUCT(std::move(resultFields));
    auto bindData =
        std::make_unique<PropertiesBindData>(std::move(returnType), std::move(fieldIndices));
    bindData->paramTypes.push_back(input.arguments[0]->getDataType().copy());
    return bindData;
}

static void singleArgCompileFunc(FunctionBindData* bindData,
    const std::vector<std::shared_ptr<ValueVector>>& parameters,
    std::shared_ptr<ValueVector>& result) {
    DASSERT(parameters[0]->dataType.getPhysicalType() == PhysicalTypeID::STRUCT);
    // Contract:
    // If parameters[0] and result share the same DataChunkState (the common unflat pipeline case),
    // child vectors can be referenced directly without copying property data.
    // In this case, singleArgExecFunc only propagates the outer null mask.
    // Otherwise, singleArgExecFunc performs a flat-to-unflat copy of all property values and null
    // masks.
    if (parameters[0]->state.get() == result->state.get()) {
        auto& propertiesBindData = bindData->cast<PropertiesBindData>();
        for (auto i = 0u; i < propertiesBindData.fieldIndices.size(); ++i) {
            auto inFieldVector = StructVector::getFieldVector(parameters[0].get(),
                propertiesBindData.fieldIndices[i]);
            StructVector::referenceVector(result.get(), i, inFieldVector);
        }
    }
}

static void copyParameterValueToStructFieldVector(const ValueVector* parameter,
    ValueVector* structField, DataChunkState* structVectorState) {
    DASSERT(parameter->state->isFlat());
    auto paramPos = parameter->state->getSelVector()[0];
    if (structVectorState->isFlat()) {
        auto pos = structVectorState->getSelVector()[0];
        structField->copyFromVectorData(pos, parameter, paramPos);
    } else {
        for (auto i = 0u; i < structVectorState->getSelVector().getSelSize(); i++) {
            auto pos = structVectorState->getSelVector()[i];
            structField->copyFromVectorData(pos, parameter, paramPos);
        }
    }
}

static void singleArgExecFunc(const std::vector<std::shared_ptr<common::ValueVector>>& parameters,
    const std::vector<common::SelectionVector*>& parameterSelVectors, common::ValueVector& result,
    common::SelectionVector* resultSelVector, void* dataPtr) {
    auto* bindData = static_cast<PropertiesBindData*>(dataPtr);
    auto* parameterSelVector = parameterSelVectors[0];

    // Reference path: child vectors are already referenced by singleArgCompileFunc.
    // We only need to propagate the outer null mask.
    if (parameterSelVector == resultSelVector) {
        if (parameters[0]->state->isFlat()) {
            auto pos = (*parameterSelVector)[0];
            result.setNull(pos, parameters[0]->isNull(pos));
        } else {
            if (parameters[0]->hasNoNullsGuarantee()) {
                result.setAllNonNull();
            } else if (parameterSelVector->isUnfiltered()) {
                result.setNullFromBits(parameters[0]->getNullMask().getData(), 0, 0,
                    parameterSelVector->getSelSize());
            } else {
                for (auto i = 0u; i < parameterSelVector->getSelSize(); i++) {
                    auto pos = (*parameterSelVector)[i];
                    result.setNull(pos, parameters[0]->isNull(pos));
                }
            }
        }
        return;
    }

    // Copy path (e.g. flat parameter into unflat result):
    DASSERT(parameters[0]->state->isFlat());
    auto paramPos = (*parameterSelVector)[0];
    auto isParamNull = parameters[0]->isNull(paramPos);
    if (result.state->isFlat()) {
        auto pos = (*resultSelVector)[0];
        result.setNull(pos, isParamNull);
    } else {
        for (auto i = 0u; i < resultSelVector->getSelSize(); i++) {
            auto pos = (*resultSelVector)[i];
            result.setNull(pos, isParamNull);
        }
    }

    for (auto i = 0u; i < bindData->fieldIndices.size(); i++) {
        auto inFieldVector =
            StructVector::getFieldVector(parameters[0].get(), bindData->fieldIndices[i]);
        auto outFieldVector = StructVector::getFieldVector(&result, i);
        outFieldVector->resetAuxiliaryBuffer();
        copyParameterValueToStructFieldVector(inFieldVector.get(), outFieldVector.get(),
            result.state.get());
    }
}

function_set PropertiesFunction::getFunctionSet() {
    function_set functions;
    auto listFunc = std::make_unique<ScalarFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::LIST, LogicalTypeID::STRING}, LogicalTypeID::ANY,
        execFunc);
    listFunc->bindFunc = bindFunc;
    listFunc->compileFunc = compileFunc;
    functions.push_back(std::move(listFunc));

    auto singleArgInputTypes =
        std::vector<LogicalTypeID>{LogicalTypeID::NODE, LogicalTypeID::REL, LogicalTypeID::STRUCT};
    for (auto inputTypeID : singleArgInputTypes) {
        auto singleArgFunc = std::make_unique<ScalarFunction>(name,
            std::vector<LogicalTypeID>{inputTypeID}, LogicalTypeID::STRUCT, singleArgExecFunc);
        singleArgFunc->bindFunc = singleArgBindFunc;
        singleArgFunc->compileFunc = singleArgCompileFunc;
        functions.push_back(std::move(singleArgFunc));
    }
    return functions;
}

} // namespace function
} // namespace lbug

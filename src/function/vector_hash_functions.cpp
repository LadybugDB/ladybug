#include "function/hash/vector_hash_functions.h"

#include "common/data_chunk/sel_vector.h"
#include "common/system_config.h"
#include "common/type_utils.h"
#include "function/hash/hash_functions.h"
#include "function/scalar_function.h"

using namespace lbug::common;

namespace lbug {
namespace function {

template<typename OPERAND_TYPE, typename RESULT_TYPE>
static void executeOnValue(const ValueVector& operand, sel_t operandPos, ValueVector& result,
    sel_t resultPos) {
    Hash::operation(operand.getValue<OPERAND_TYPE>(operandPos),
        result.getValue<RESULT_TYPE>(resultPos));
}

template<typename OPERAND_TYPE, typename RESULT_TYPE>
void UnaryHashFunctionExecutor::execute(const ValueVector& operand,
    const SelectionView& operandSelectVec, ValueVector& result,
    const SelectionView& resultSelectVec) {
    auto resultValues = (RESULT_TYPE*)result.getData();
    if (operand.hasNoNullsGuarantee()) {
        if (operandSelectVec.isUnfiltered()) {
            for (auto i = 0u; i < operandSelectVec.getSelSize(); i++) {
                auto resultPos = resultSelectVec[i];
                executeOnValue<OPERAND_TYPE, RESULT_TYPE>(operand, i, result, resultPos);
            }
        } else {
            for (auto i = 0u; i < operandSelectVec.getSelSize(); i++) {
                auto operandPos = operandSelectVec[i];
                auto resultPos = resultSelectVec[i];
                Hash::operation(operand.getValue<OPERAND_TYPE>(operandPos),
                    resultValues[resultPos]);
            }
        }
    } else {
        if (operandSelectVec.isUnfiltered()) {
            for (auto i = 0u; i < operandSelectVec.getSelSize(); i++) {
                auto resultPos = resultSelectVec[i];
                if (!operand.isNull(i)) {
                    Hash::operation(operand.getValue<OPERAND_TYPE>(i), resultValues[resultPos]);
                } else {
                    result.setValue(resultPos, NULL_HASH);
                }
            }
        } else {
            for (auto i = 0u; i < operandSelectVec.getSelSize(); i++) {
                auto operandPos = operandSelectVec[i];
                auto resultPos = resultSelectVec[i];
                if (!operand.isNull(operandPos)) {
                    Hash::operation(operand.getValue<OPERAND_TYPE>(operandPos),
                        resultValues[resultPos]);
                } else {
                    result.setValue(resultPos, NULL_HASH);
                }
            }
        }
    }
}

template<typename LEFT_TYPE, typename RIGHT_TYPE, typename RESULT_TYPE, typename FUNC>
static void executeOnValue(const common::ValueVector& left, common::sel_t leftPos,
    const common::ValueVector& right, common::sel_t rightPos, common::ValueVector& result,
    common::sel_t resultPos) {
    FUNC::operation(left.getValue<LEFT_TYPE>(leftPos), right.getValue<RIGHT_TYPE>(rightPos),
        result.getValue<RESULT_TYPE>(resultPos));
}

static void validateSelState(const SelectionView& leftSelVec, const SelectionView& rightSelVec,
    const SelectionView& resultSelVec) {
    auto leftSelSize = leftSelVec.getSelSize();
    auto rightSelSize = rightSelVec.getSelSize();
    auto resultSelSize = resultSelVec.getSelSize();
    (void)resultSelSize;
    if (leftSelSize > 1 && rightSelSize > 1) {
        DASSERT(leftSelSize == rightSelSize);
        DASSERT(leftSelSize == resultSelSize);
    } else if (leftSelSize > 1) {
        DASSERT(leftSelSize == resultSelSize);
    } else if (rightSelSize > 1) {
        DASSERT(rightSelSize == resultSelSize);
    }
}

template<typename LEFT_TYPE, typename RIGHT_TYPE, typename RESULT_TYPE, typename FUNC>
void BinaryHashFunctionExecutor::execute(const common::ValueVector& left,
    const SelectionView& leftSelVec, const common::ValueVector& right,
    const SelectionView& rightSelVec, common::ValueVector& result,
    const SelectionView& resultSelVec) {
    validateSelState(leftSelVec, rightSelVec, resultSelVec);
    result.resetAuxiliaryBuffer();
    if (leftSelVec.getSelSize() != 1 && rightSelVec.getSelSize() != 1) {
        for (auto i = 0u; i < leftSelVec.getSelSize(); i++) {
            auto leftPos = leftSelVec[i];
            auto rightPos = rightSelVec[i];
            auto resultPos = resultSelVec[i];
            executeOnValue<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, FUNC>(left, leftPos, right, rightPos,
                result, resultPos);
        }
    } else if (leftSelVec.getSelSize() == 1) {
        auto leftPos = leftSelVec[0];
        for (auto i = 0u; i < rightSelVec.getSelSize(); i++) {
            auto rightPos = rightSelVec[i];
            auto resultPos = resultSelVec[i];
            executeOnValue<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, FUNC>(left, leftPos, right, rightPos,
                result, resultPos);
        }
    } else {
        auto rightPos = rightSelVec[0];
        for (auto i = 0u; i < leftSelVec.getSelSize(); i++) {
            auto leftPos = leftSelVec[i];
            auto resultPos = resultSelVec[i];
            executeOnValue<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, FUNC>(left, leftPos, right, rightPos,
                result, resultPos);
        }
    }
}

static std::unique_ptr<ValueVector> computeDataVecHash(const ValueVector& operand,
    const SelectionView& operandSelVec, uint64_t& rangeBaseOut) {
    // Hash only the child entries covered by the selected rows. Hashing the entire
    // backing data vector on every call is pathological when list hashing runs
    // row-at-a-time (e.g. multi-key hash-join probes force flat keys): each call
    // re-hashed the whole store plus allocated a full-size buffer for it
    // (LDBC SNB Q10 spent ~11s parallel-sum hashing one constant 18-element list
    // per probe row). The covered union is all finalizeDataVecHash ever reads, so
    // the produced hashes are identical.
    uint64_t minOffset = UINT64_MAX;
    uint64_t maxOffset = 0;
    for (auto i = 0u; i < operandSelVec.getSelSize(); i++) {
        auto pos = operandSelVec[i];
        if (operand.isNull(pos)) {
            continue;
        }
        auto entry = operand.getValue<list_entry_t>(pos);
        if (entry.size == 0) {
            continue;
        }
        minOffset = std::min(minOffset, static_cast<uint64_t>(entry.offset));
        maxOffset = std::max(maxOffset, static_cast<uint64_t>(entry.offset) + entry.size);
    }
    auto hashVector = std::make_unique<ValueVector>(LogicalType::LIST(LogicalType::HASH()));
    if (maxOffset <= minOffset) {
        rangeBaseOut = 0;
        return hashVector;
    }
    rangeBaseOut = minOffset;
    auto rangeLen = maxOffset - minOffset;
    ListVector::resizeDataVector(hashVector.get(), rangeLen);
    // TODO(Ziyi): Allow selection size to be greater than default vector capacity, so we don't have
    // to chunk the selectionVector.
    SelectionVector srcSelection{DEFAULT_VECTOR_CAPACITY};
    SelectionVector dstSelection{DEFAULT_VECTOR_CAPACITY};
    srcSelection.setToFiltered();
    dstSelection.setToFiltered();
    auto numValuesComputed = 0u;
    uint64_t numValuesToComputeHash = 0;
    while (numValuesComputed < rangeLen) {
        numValuesToComputeHash =
            std::min<uint64_t>(DEFAULT_VECTOR_CAPACITY, rangeLen - numValuesComputed);
        for (auto i = 0u; i < numValuesToComputeHash; i++) {
            srcSelection[i] = static_cast<sel_t>(minOffset + numValuesComputed);
            dstSelection[i] = static_cast<sel_t>(numValuesComputed);
            numValuesComputed++;
        }
        srcSelection.setSelSize(numValuesToComputeHash);
        dstSelection.setSelSize(numValuesToComputeHash);
        VectorHashFunction::computeHash(*ListVector::getDataVector(&operand), srcSelection,
            *ListVector::getDataVector(hashVector.get()), dstSelection);
    }
    return hashVector;
}

static void finalizeDataVecHash(const ValueVector& operand, const SelectionView& operandSelVec,
    ValueVector& result, const SelectionView& resultSelVec, ValueVector& tmpHashVec,
    uint64_t rangeBase) {
    for (auto i = 0u; i < operandSelVec.getSelSize(); i++) {
        auto pos = operandSelVec[i];
        auto resultPos = resultSelVec[i];
        auto entry = operand.getValue<list_entry_t>(pos);
        if (operand.isNull(pos)) {
            result.setValue(resultPos, NULL_HASH);
        } else {
            auto hashValue = NULL_HASH;
            for (auto j = 0u; j < entry.size; j++) {
                hashValue = combineHashScalar(hashValue,
                    ListVector::getDataVector(&tmpHashVec)
                        ->getValue<hash_t>(entry.offset + j - rangeBase));
            }
            result.setValue(resultPos, hashValue);
        }
    }
}

static void computeListVectorHash(const ValueVector& operand, const SelectionView& operandSelectVec,
    ValueVector& result, const SelectionView& resultSelectVec) {
    uint64_t rangeBase = 0;
    auto dataVecHash = computeDataVecHash(operand, operandSelectVec, rangeBase);
    finalizeDataVecHash(operand, operandSelectVec, result, resultSelectVec, *dataVecHash,
        rangeBase);
}

static void computeStructVecHash(const ValueVector& operand, const SelectionView& operandSelVec,
    ValueVector& result, const SelectionView& resultSelVec) {
    switch (operand.dataType.getLogicalTypeID()) {
    case LogicalTypeID::NODE: {
        DASSERT(0 == common::StructType::getFieldIdx(operand.dataType, InternalKeyword::ID));
        UnaryHashFunctionExecutor::execute<internalID_t, hash_t>(
            *StructVector::getFieldVector(&operand, 0), operandSelVec, result, resultSelVec);
    } break;
    case LogicalTypeID::REL: {
        DASSERT(3 == StructType::getFieldIdx(operand.dataType, InternalKeyword::ID));
        UnaryHashFunctionExecutor::execute<internalID_t, hash_t>(
            *StructVector::getFieldVector(&operand, 3), operandSelVec, result, resultSelVec);
    } break;
    case LogicalTypeID::RECURSIVE_REL:
    case LogicalTypeID::UNION:
    case LogicalTypeID::STRUCT: {
        VectorHashFunction::computeHash(*StructVector::getFieldVector(&operand, 0 /* idx */),
            operandSelVec, result, resultSelVec);
        auto tmpHashVector = std::make_unique<ValueVector>(LogicalType::HASH());
        SelectionView tmpSel(resultSelVec.getSelSize());
        for (auto i = 1u; i < StructType::getNumFields(operand.dataType); i++) {
            auto fieldVector = StructVector::getFieldVector(&operand, i);
            VectorHashFunction::computeHash(*fieldVector, operandSelVec, *tmpHashVector, tmpSel);
            VectorHashFunction::combineHash(*tmpHashVector, tmpSel, result, resultSelVec, result,
                resultSelVec);
        }
    } break;
    default:
        UNREACHABLE_CODE;
    }
}

void VectorHashFunction::computeHash(const ValueVector& operand,
    const SelectionView& operandSelectVec, ValueVector& result,
    const SelectionView& resultSelectVec) {
    result.state = operand.state;
    DASSERT(result.dataType.getLogicalTypeID() == LogicalType::HASH().getLogicalTypeID());
    TypeUtils::visit(
        operand.dataType.getPhysicalType(),
        [&]<HashableNonNestedTypes T>(T) {
            UnaryHashFunctionExecutor::execute<T, hash_t>(operand, operandSelectVec, result,
                resultSelectVec);
        },
        [&](struct_entry_t) {
            computeStructVecHash(operand, operandSelectVec, result, resultSelectVec);
        },
        [&](list_entry_t) {
            computeListVectorHash(operand, operandSelectVec, result, resultSelectVec);
        },
        [&operand](auto) {
            // LCOV_EXCL_START
            throw RuntimeException("Cannot hash data type " + operand.dataType.toString());
            // LCOV_EXCL_STOP
        });
}

void VectorHashFunction::combineHash(const ValueVector& left, const SelectionView& leftSelVec,
    const ValueVector& right, const SelectionView& rightSelVec, ValueVector& result,
    const SelectionView& resultSelVec) {
    DASSERT(left.dataType.getLogicalTypeID() == LogicalType::HASH().getLogicalTypeID());
    DASSERT(left.dataType.getLogicalTypeID() == right.dataType.getLogicalTypeID());
    DASSERT(left.dataType.getLogicalTypeID() == result.dataType.getLogicalTypeID());
    BinaryHashFunctionExecutor::execute<hash_t, hash_t, hash_t, CombineHash>(left, leftSelVec,
        right, rightSelVec, result, resultSelVec);
}

static void HashExecFunc(const std::vector<std::shared_ptr<common::ValueVector>>& params,
    const std::vector<common::SelectionVector*>& paramSelVectors, common::ValueVector& result,
    common::SelectionVector*, void* /*dataPtr*/ = nullptr) {
    DASSERT(params.size() == 1);
    // TODO(Ziyi): evaluators should resolve the state for result vector.
    result.state = params[0]->state;
    VectorHashFunction::computeHash(*params[0], *paramSelVectors[0], result,
        result.state->getSelVectorUnsafe());
}

function_set HashFunction::getFunctionSet() {
    function_set functionSet;
    functionSet.push_back(std::make_unique<ScalarFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::ANY}, LogicalTypeID::UINT64, HashExecFunc));
    return functionSet;
}

} // namespace function
} // namespace lbug

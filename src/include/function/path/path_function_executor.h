#pragma once

#include "common/constants.h"
#include "common/vector/value_vector.h"
#include "function/hash/hash_functions.h"

namespace lbug {
namespace function {

static bool isAllInternalIDDistinct(common::ValueVector* dataVector, common::offset_t startOffset,
    uint64_t size, std::unordered_set<common::internalID_t, InternalIDHasher>& internalIDSet) {
    internalIDSet.clear();
    for (auto i = 0u; i < size; ++i) {
        auto& internalID = dataVector->getValue<common::internalID_t>(startOffset + i);
        if (internalIDSet.contains(internalID)) {
            return false;
        }
        internalIDSet.insert(internalID);
    }
    return true;
}

// Note: this executor is only used for isTrail and isAcyclic. So we add some ad-hoc optimization
// into executor, e.g. internalIDSet. A more general implementation can be done once needed. But
// pay attention to the performance drop. Depends on how bad it becomes, we may want to implement
// customized executors.
struct UnaryPathExecutor {
    static void executeNodeIDs(common::ValueVector& input, common::SelectionVector& inputSelVector,
        common::ValueVector& result, common::SelectionVector& resultSelVector) {
        auto nodesFieldIdx = 0;
        DASSERT(nodesFieldIdx ==
                common::StructType::getFieldIdx(input.dataType, common::InternalKeyword::NODES));
        auto nodesVector = common::StructVector::getFieldVector(&input, nodesFieldIdx).get();
        auto internalIDFieldIdx = 0;
        execute(input, inputSelVector, *nodesVector, internalIDFieldIdx, result, resultSelVector);
    }

    static void executeRelIDs(common::ValueVector& input, common::SelectionVector& inputSelVector,
        common::ValueVector& result, common::SelectionVector& resultSelVector) {
        auto relsFieldIdx = 1;
        DASSERT(relsFieldIdx ==
                common::StructType::getFieldIdx(input.dataType, common::InternalKeyword::RELS));
        auto relsVector = common::StructVector::getFieldVector(&input, relsFieldIdx).get();
        auto internalIDFieldIdx = 3;
        execute(input, inputSelVector, *relsVector, internalIDFieldIdx, result, resultSelVector);
    }

    static bool selectNodeIDs(common::ValueVector& input,
        common::SelectionVector& selectionVector) {
        auto nodesFieldIdx = 0;
        DASSERT(nodesFieldIdx ==
                common::StructType::getFieldIdx(input.dataType, common::InternalKeyword::NODES));
        auto nodesVector = common::StructVector::getFieldVector(&input, nodesFieldIdx).get();
        auto internalIDFieldIdx = 0;
        return select(input, *nodesVector, internalIDFieldIdx, selectionVector);
    }

    static bool selectRelIDs(common::ValueVector& input, common::SelectionVector& selectionVector) {
        auto relsFieldIdx = 1;
        DASSERT(relsFieldIdx ==
                common::StructType::getFieldIdx(input.dataType, common::InternalKeyword::RELS));
        auto relsVector = common::StructVector::getFieldVector(&input, relsFieldIdx).get();
        auto internalIDFieldIdx = 3;
        return select(input, *relsVector, internalIDFieldIdx, selectionVector);
    }

private:
    static common::ValueVector* getInternalIDsVector(common::ValueVector& listVector,
        common::struct_field_idx_t fieldIdx) {
        auto listDataVector = common::ListVector::getDataVector(&listVector);
        DASSERT(fieldIdx == common::StructType::getFieldIdx(listDataVector->dataType,
                                common::InternalKeyword::ID));
        return common::StructVector::getFieldVector(listDataVector, fieldIdx).get();
    }

    // A flat input's position can differ from the result's, as in UnaryFunctionExecutor. A null
    // path or recursive rel (e.g. from an OPTIONAL MATCH that found nothing) gives null.
    static void execute(const common::ValueVector& input,
        const common::SelectionVector& inputSelVector, common::ValueVector& listVector,
        common::struct_field_idx_t fieldIdx, common::ValueVector& result,
        const common::SelectionVector& resultSelVector) {
        auto internalIDsVector = getInternalIDsVector(listVector, fieldIdx);
        std::unordered_set<common::nodeID_t, InternalIDHasher> internalIDSet;
        for (auto i = 0u; i < inputSelVector.getSelSize(); ++i) {
            auto inputPos = inputSelVector[i];
            auto resultPos = resultSelVector[i];
            result.setNull(resultPos, input.isNull(inputPos));
            if (result.isNull(resultPos)) {
                continue;
            }
            auto& listEntry = listVector.getValue<common::list_entry_t>(inputPos);
            result.setValue<bool>(resultPos, isAllInternalIDDistinct(internalIDsVector,
                                                 listEntry.offset, listEntry.size, internalIDSet));
        }
    }

    // A null path or recursive rel is not selected.
    static bool select(const common::ValueVector& input, common::ValueVector& listVector,
        common::struct_field_idx_t fieldIdx, common::SelectionVector& selectionVector) {
        auto& inputSelVector = input.state->getSelVector();
        auto internalIDsVector = getInternalIDsVector(listVector, fieldIdx);
        std::unordered_set<common::nodeID_t, InternalIDHasher> internalIDSet;
        auto numSelectedValues = 0u;
        auto buffer = selectionVector.getMutableBuffer();
        for (auto i = 0u; i < inputSelVector.getSelSize(); ++i) {
            auto pos = inputSelVector[i];
            if (input.isNull(pos)) {
                continue;
            }
            auto& listEntry = listVector.getValue<common::list_entry_t>(pos);
            buffer[numSelectedValues] = pos;
            numSelectedValues += isAllInternalIDDistinct(internalIDsVector, listEntry.offset,
                listEntry.size, internalIDSet);
        }
        selectionVector.setSelSize(numSelectedValues);
        return numSelectedValues > 0;
    }
};

} // namespace function
} // namespace lbug

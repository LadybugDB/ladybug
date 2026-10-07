#include "processor/operator/hash_join/join_hash_table.h"

#include <algorithm>
#include <cstring>

#include "common/utils.h"
#include "function/hash/vector_hash_functions.h"
#include "processor/result/factorized_table.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::function;

namespace lbug {
namespace processor {

JoinHashTable::JoinHashTable(MemoryManager& memoryManager, logical_type_vec_t keyTypes,
    FactorizedTableSchema tableSchema)
    : BaseHashTable{memoryManager, std::move(keyTypes)} {
    auto numSlotsPerBlock = HASH_BLOCK_SIZE / sizeof(uint8_t*);
    initSlotConstant(numSlotsPerBlock);
    // Prev pointer is always the last column in the table.
    prevPtrColOffset = tableSchema.getColOffset(tableSchema.getNumColumns() - PREV_PTR_COL_IDX);
    factorizedTable = std::make_unique<FactorizedTable>(&memoryManager, std::move(tableSchema));
}

// Returns false if no tuple has all keys non-null. Only the unflat keys' state is compacted
// (appendVectors and the probe operator restore it); a flat key's state is left alone, since its
// producer keeps writing through that selection vector.
static bool discardNullFromKeys(const std::vector<ValueVector*>& vectors) {
    for (auto& vector : vectors) {
        if (vector->state->isFlat()) {
            const auto& selVector = vector->state->getSelVector();
            if (selVector.getSelSize() == 0 || vector->isNull(selVector[0])) {
                return false;
            }
        } else if (!ValueVector::discardNull(*vector)) {
            return false;
        }
    }
    return true;
}

static bool mayHaveNullUnflatKeys(const std::vector<ValueVector*>& keyVectors) {
    return std::any_of(keyVectors.begin(), keyVectors.end(), [](const ValueVector* vector) {
        return !vector->state->isFlat() && !vector->hasNoNullsGuarantee();
    });
}

static void copySelVector(const SelectionVector& from, SelectionVector& to) {
    if (from.isUnfiltered()) {
        to.setToUnfiltered(from.getSelSize());
    } else {
        std::memcpy(to.getMutableBuffer().data(), from.getSelectedPositions().data(),
            from.getSelSize() * sizeof(sel_t));
        to.setToFiltered(from.getSelSize());
    }
}

uint64_t JoinHashTable::appendVectors(const std::vector<ValueVector*>& keyVectors,
    const std::vector<ValueVector*>& payloadVectors, DataChunkState* keyState) {
    // Discarding null keys compacts the unflat key state's selection vector, which the child
    // writes its next batch through. Compact a copy and give the child's back afterwards.
    if (!mayHaveNullUnflatKeys(keyVectors)) {
        return appendVectorsDiscardingNulls(keyVectors, payloadVectors, keyState);
    }
    if (nullFreeSelVector == nullptr) {
        nullFreeSelVector = std::make_shared<SelectionVector>(DEFAULT_VECTOR_CAPACITY);
    }
    auto childSelVector = keyState->getSelVectorShared();
    copySelVector(*childSelVector, *nullFreeSelVector);
    keyState->setSelVector(nullFreeSelVector);
    const auto numAppended = appendVectorsDiscardingNulls(keyVectors, payloadVectors, keyState);
    keyState->setSelVector(std::move(childSelVector));
    return numAppended;
}

uint64_t JoinHashTable::appendVectorsDiscardingNulls(const std::vector<ValueVector*>& keyVectors,
    const std::vector<ValueVector*>& payloadVectors, DataChunkState* keyState) {
    if (!discardNullFromKeys(keyVectors)) {
        return 0;
    }
    auto numTuplesToAppend = keyState->getSelVector().getSelSize();
    auto appendInfos = factorizedTable->allocateFlatTupleBlocks(numTuplesToAppend);
    computeVectorHashes(keyVectors);
    auto colIdx = 0u;
    for (auto& vector : keyVectors) {
        appendVector(vector, appendInfos, colIdx++);
    }
    for (auto& vector : payloadVectors) {
        appendVector(vector, appendInfos, colIdx++);
    }
    appendVector(hashVector.get(), appendInfos, colIdx);
    factorizedTable->numTuples += numTuplesToAppend;
    return numTuplesToAppend;
}

void JoinHashTable::appendVector(ValueVector* vector,
    const std::vector<BlockAppendingInfo>& appendInfos, ft_col_idx_t colIdx) {
    auto numAppendedTuples = 0ul;
    for (auto& blockAppendInfo : appendInfos) {
        factorizedTable->copyVectorToColumn(*vector, blockAppendInfo, numAppendedTuples, colIdx);
        numAppendedTuples += blockAppendInfo.numTuplesToAppend;
    }
}

static void sortSelectedPos(ValueVector* nodeIDVector) {
    auto& selVector = nodeIDVector->state->getSelVectorUnsafe();
    auto size = selVector.getSelSize();
    auto buffer = selVector.getMutableBuffer();
    if (selVector.isUnfiltered()) {
        std::memcpy(buffer.data(), selVector.getSelectedPositions().data(), size * sizeof(sel_t));
        selVector.setToFiltered();
    }
    std::sort(buffer.begin(), buffer.begin() + size, [nodeIDVector](sel_t left, sel_t right) {
        return nodeIDVector->getValue<nodeID_t>(left) < nodeIDVector->getValue<nodeID_t>(right);
    });
}

uint64_t JoinHashTable::appendVectorWithSorting(ValueVector* keyVector,
    std::vector<ValueVector*> payloadVectors) {
    auto numTuplesToAppend = 1;
    DASSERT(keyVector->state->getSelVector().getSelSize() == 1);
    // Based on the way we are planning, we assume that the first and second vectors are both
    // nodeIDs from extending, while the first one is key, and the second one is payload.
    auto payloadNodeIDVector = payloadVectors[0];
    auto payloadsState = payloadNodeIDVector->state.get();
    if (!payloadsState->isFlat()) {
        // Sorting is only needed when the payload is unFlat (a list of values).
        sortSelectedPos(payloadNodeIDVector);
    }
    // A single appendInfo will return from `allocateFlatTupleBlocks` when numTuplesToAppend is 1.
    auto appendInfos = factorizedTable->allocateFlatTupleBlocks(numTuplesToAppend);
    DASSERT(appendInfos.size() == 1);
    auto colIdx = 0u;
    std::vector<ValueVector*> keyVectors = {keyVector};
    computeVectorHashes(keyVectors);
    factorizedTable->copyVectorToColumn(*keyVector, appendInfos[0], numTuplesToAppend, colIdx++);
    for (auto& vector : payloadVectors) {
        factorizedTable->copyVectorToColumn(*vector, appendInfos[0], numTuplesToAppend, colIdx++);
    }
    factorizedTable->copyVectorToColumn(*hashVector, appendInfos[0], numTuplesToAppend, colIdx);
    if (!payloadsState->isFlat()) {
        // TODO(Xiyang): I can no longer recall why I set to un-filtered but this is probably wrong.
        // We should set back to the un-sorted state.
        payloadsState->getSelVectorUnsafe().setToUnfiltered();
    }
    factorizedTable->numTuples += numTuplesToAppend;
    return numTuplesToAppend;
}

void JoinHashTable::allocateHashSlots(uint64_t numTuples) {
    setMaxNumHashSlots(nextPowerOfTwo(numTuples * 2));
    auto numSlotsPerBlock = (uint64_t)1 << numSlotsPerBlockLog2;
    auto numBlocksNeeded = (maxNumHashSlots + numSlotsPerBlock - 1) / numSlotsPerBlock;
    while (hashSlotsBlocks.size() < numBlocksNeeded) {
        hashSlotsBlocks.emplace_back(std::make_unique<DataBlock>(memoryManager, HASH_BLOCK_SIZE));
    }
}

void JoinHashTable::buildHashSlots() {
    for (auto& tupleBlock : factorizedTable->getTupleDataBlocks()) {
        uint8_t* tuple = tupleBlock->getData();
        for (auto i = 0u; i < tupleBlock->numTuples; i++) {
            auto lastSlotEntryInHT = insertEntry(tuple);
            auto prevPtr = tuple + prevPtrColOffset;
            memcpy(reinterpret_cast<void*>(prevPtr), reinterpret_cast<void*>(&lastSlotEntryInHT),
                sizeof(uint8_t*));
            tuple += getTableSchema()->getNumBytesPerTuple();
        }
    }
}

void JoinHashTable::probe(const std::vector<ValueVector*>& keyVectors, ValueVector& hashVector,
    SelectionVector& hashSelVec, ValueVector* tmpHashResultVector, uint8_t** probedTuples) {
    DASSERT(keyVectors.size() == keyTypes.size());
    if (getNumEntries() == 0) {
        return;
    }
    if (!discardNullFromKeys(keyVectors)) {
        // A null flat key keeps its selection size, so drop the chain left by the previous probe.
        probedTuples[0] = nullptr;
        return;
    }
    hashSelVec.setSelSize(keyVectors[0]->state->getSelVector().getSelSize());
    VectorHashFunction::computeHash(*keyVectors[0], keyVectors[0]->state->getSelVector(),
        hashVector, hashSelVec);
    for (auto i = 1u; i < keyVectors.size(); i++) {
        hashSelVec.setSelSize(keyVectors[i]->state->getSelVector().getSelSize());
        VectorHashFunction::computeHash(*keyVectors[i], keyVectors[i]->state->getSelVector(),
            *tmpHashResultVector, hashSelVec);
        VectorHashFunction::combineHash(hashVector, hashSelVec, *tmpHashResultVector, hashSelVec,
            hashVector, hashSelVec);
    }
    for (auto i = 0u; i < hashSelVec.getSelSize(); i++) {
        DASSERT(i < DEFAULT_VECTOR_CAPACITY);
        probedTuples[i] = getTupleForHash(hashVector.getValue<hash_t>(hashSelVec[i]));
    }
}

sel_t JoinHashTable::matchFlatKeys(const std::vector<ValueVector*>& keyVectors,
    uint8_t** probedTuples, uint8_t** matchedTuples) {
    auto numMatchedTuples = 0;
    while (probedTuples[0]) {
        if (numMatchedTuples == DEFAULT_VECTOR_CAPACITY) {
            break;
        }
        auto currentTuple = probedTuples[0];
        matchedTuples[numMatchedTuples] = currentTuple;
        numMatchedTuples += matchFlatVecWithEntry(keyVectors, currentTuple);
        probedTuples[0] = getPrevTupleValue(currentTuple);
    }
    return numMatchedTuples;
}

sel_t JoinHashTable::matchUnFlatKey(ValueVector* keyVector, uint8_t** probedTuples,
    uint8_t** matchedTuples, SelectionVector& matchedTuplesSelVector) {
    auto numMatchedTuples = 0;
    for (auto i = 0u; i < keyVector->state->getSelVector().getSelSize(); ++i) {
        auto pos = keyVector->state->getSelVector()[i];
        while (probedTuples[i]) {
            auto currentTuple = probedTuples[i];
            auto entryCompareResult = compareEntryFuncs[0](keyVector, pos, currentTuple);
            if (entryCompareResult) {
                matchedTuples[numMatchedTuples] = currentTuple;
                matchedTuplesSelVector[numMatchedTuples] = pos;
                numMatchedTuples++;
                break;
            }
            probedTuples[i] = getPrevTupleValue(currentTuple);
        }
    }
    return numMatchedTuples;
}

uint8_t** JoinHashTable::findHashSlot(const uint8_t* tuple) const {
    // Hash column lives in a packed factorized-table tuple with no alignment padding.
    hash_t hash;
    memcpy(&hash, tuple + getHashValueColOffset(), sizeof(hash_t));
    auto slotIdx = getSlotIdxForHash(hash);
    return (uint8_t**)(hashSlotsBlocks[slotIdx >> numSlotsPerBlockLog2]->getData() +
                       (slotIdx & slotIdxInBlockMask) * sizeof(uint8_t*));
}

uint8_t* JoinHashTable::insertEntry(uint8_t* tuple) const {
    auto slot = findHashSlot(tuple);
    auto prevPtr = *slot;
    *slot = tuple;
    return prevPtr;
}

void JoinHashTable::computeVectorHashes(std::vector<common::ValueVector*> keyVectors) {
    BaseHashTable::computeVectorHashes(keyVectors);
}

offset_t JoinHashTable::getHashValueColOffset() const {
    return getTableSchema()->getColOffset(getTableSchema()->getNumColumns() - HASH_COL_IDX);
}

} // namespace processor
} // namespace lbug

#include "processor/result/result_set.h"

using namespace lbug::common;

namespace lbug {
namespace processor {

ResultSet::ResultSet(ResultSetDescriptor* resultSetDescriptor,
    storage::MemoryManager* memoryManager)
    : multiplicity{1} {
    auto numDataChunks = resultSetDescriptor->dataChunkDescriptors.size();
    dataChunks.resize(numDataChunks);
    for (auto i = 0u; i < numDataChunks; ++i) {
        auto dataChunkDescriptor = resultSetDescriptor->dataChunkDescriptors[i].get();
        auto numValueVectors = dataChunkDescriptor->logicalTypes.size();
        auto dataChunk = std::make_unique<DataChunk>(numValueVectors);
        if (dataChunkDescriptor->isSingleState) {
            dataChunk->state = DataChunkState::getSingleValueDataChunkState();
        }
        for (auto j = 0u; j < numValueVectors; ++j) {
            auto vector = std::make_shared<ValueVector>(dataChunkDescriptor->logicalTypes[j].copy(),
                memoryManager);
            dataChunk->insert(j, std::move(vector));
        }
        insert(i, std::move(dataChunk));
    }
}

uint64_t ResultSet::getNumTuplesWithoutMultiplicity(
    const std::unordered_set<uint32_t>& dataChunksPosInScope) {
    DASSERT(!dataChunksPosInScope.empty());
    uint64_t numTuples = 1;
    for (auto& dataChunkPos : dataChunksPosInScope) {
        numTuples *= dataChunks[dataChunkPos]->state->getSelVector().getSelSize();
    }
    return numTuples;
}

void ResultSet::resetForReuse() {
    // multiplicity is overwritten by Projection / MultiplicityReducer each
    // execution, but if neither runs (e.g. trivial ``RETURN $i``) the cached
    // value from the previous run would leak into the next. Reset to 1 so
    // downstream operators that multiply it start from a clean slate.
    multiplicity = 1;
    for (auto& dataChunk : dataChunks) {
        if (dataChunk == nullptr) {
            continue;
        }
        // Restore the DataChunkState to its freshly-constructed condition: identity
        // (unfiltered) selection with size 1 for flat / 0 for unflat states, and no packed
        // child slices. Producers set the real selection for each run; without this, stale
        // filtered positions or packed slices from a prior execution would be observed by
        // operators that assume a fresh state.
        auto& state = dataChunk->state;
        state->clearPackedChildSlices();
        state->getSelVectorUnsafe().setToUnfiltered(state->isFlat() ? 1 : 0);
        for (auto& valueVector : dataChunk->valueVectors) {
            // setAllNonNull() clears any null bits the previous run may have
            // set (operators that only call setValue<T>() don't touch the null
            // mask, so the stale bit would otherwise persist across runs).
            // resetAuxiliaryBuffer() drops the in-memory overflow buffer for
            // strings / lists / arrays / structs so variable-length state from
            // a prior run doesn't survive.
            valueVector->setAllNonNull();
            valueVector->resetAuxiliaryBuffer();
        }
    }
}

} // namespace processor
} // namespace lbug

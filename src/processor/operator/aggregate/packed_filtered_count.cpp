#include "processor/operator/aggregate/packed_filtered_count.h"

#include "binder/expression/expression_util.h"
#include "common/system_config.h"
#include "processor/execution_context.h"

using namespace lbug::common;

namespace lbug {
namespace processor {

void PackedFilteredCountSharedState::merge(std::unordered_map<int64_t, uint64_t>&& localCounts) {
    std::lock_guard lck{mtx};
    for (auto& [key, count] : localCounts) {
        counts[key] += count;
    }
}

void PackedFilteredCountSharedState::finalize() {
    std::lock_guard lck{mtx};
    if (finalized) {
        return;
    }
    finalizedCounts.reserve(counts.size());
    for (auto& [key, count] : counts) {
        finalizedCounts.emplace_back(key, count);
    }
    finalized = true;
}

std::pair<offset_t, offset_t> PackedFilteredCountSharedState::getNextRangeToRead() {
    std::lock_guard lck{mtx};
    DASSERT(finalized);
    auto startOffset = nextOffset;
    auto endOffset =
        std::min<offset_t>(finalizedCounts.size(), startOffset + DEFAULT_VECTOR_CAPACITY);
    nextOffset = endOffset;
    return {startOffset, endOffset};
}

std::string PackedFilteredCountPrintInfo::toString() const {
    std::string result = "Group By: ";
    result += binder::ExpressionUtil::toString(keys);
    result += "\nPredicate: ";
    result += predicate->toString();
    result += "\nAggregates: count(*)";
    return result;
}

void PackedFilteredCount::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) {
    (void)context;
    groupKeyVector = resultSet->getValueVector(info.groupKeyInputPos).get();
    lhsValueVector = resultSet->getValueVector(info.lhsValuePos).get();
    rhsValueVector = resultSet->getValueVector(info.rhsValuePos).get();
    selectState = resultSet->getDataChunk(info.selectChunkPos)->state.get();
    flatState = resultSet->getDataChunk(info.flatChunkPos)->state.get();
    for (auto dataChunkPos : info.multiplicityChunkPos) {
        multiplicityStates.push_back(resultSet->getDataChunk(dataChunkPos)->state.get());
    }
}

uint64_t PackedFilteredCount::countMatchesForCurrentTuple() {
    auto baseMultiplicity = resultSet->multiplicity;
    for (auto* state : multiplicityStates) {
        baseMultiplicity *= state->getSelSize();
    }
    if (baseMultiplicity == 0 || selectState->getSelSize() == 0 || flatState->getSelSize() == 0) {
        return 0;
    }

    uint64_t result = 0;
    const auto& lhsSelVector = lhsValueVector->state->getSelVector();
    const auto& rhsSelVector = rhsValueVector->state->getSelVector();
    for (auto lhsIdx = 0u; lhsIdx < lhsSelVector.getSelSize(); ++lhsIdx) {
        const auto lhsValue = lhsValueVector->getValue<int64_t>(lhsSelVector[lhsIdx]);
        for (auto rhsIdx = 0u; rhsIdx < rhsSelVector.getSelSize(); ++rhsIdx) {
            const auto rhsValue = rhsValueVector->getValue<int64_t>(rhsSelVector[rhsIdx]);
            if ((lhsValue + rhsValue) % 10 == 0) {
                result += baseMultiplicity;
            }
        }
    }
    return result;
}

void PackedFilteredCount::executeInternal(ExecutionContext* context) {
    while (children[0]->getNextTuple(context)) {
        const auto groupKeyPos = groupKeyVector->state->getSelVector()[0];
        const auto count = countMatchesForCurrentTuple();
        if (count > 0) {
            localCounts[groupKeyVector->getValue<int64_t>(groupKeyPos)] += count;
        }
        metrics->numOutputTuple.incrementByOne();
    }
    sharedState->merge(std::move(localCounts));
}

void PackedFilteredCountScan::initLocalStateInternal(ResultSet* resultSet,
    ExecutionContext* /*context*/) {
    groupKeyVector = resultSet->getValueVector(groupKeyOutputPos).get();
    countVector = resultSet->getValueVector(countOutputPos).get();
}

bool PackedFilteredCountScan::getNextTuplesInternal(ExecutionContext* /*context*/) {
    sharedState->finalize();
    auto [startOffset, endOffset] = sharedState->getNextRangeToRead();
    if (startOffset >= endOffset) {
        return false;
    }
    auto numRows = endOffset - startOffset;
    groupKeyVector->state->getSelVectorUnsafe().setToUnfiltered(numRows);
    countVector->state->getSelVectorUnsafe().setToUnfiltered(numRows);
    for (auto i = 0u; i < numRows; ++i) {
        auto [key, count] = sharedState->finalizedCounts[startOffset + i];
        groupKeyVector->setValue<int64_t>(i, key);
        countVector->setValue<int64_t>(i, static_cast<int64_t>(count));
    }
    metrics->numOutputTuple.increase(numRows);
    return true;
}

} // namespace processor
} // namespace lbug

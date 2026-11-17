#include "processor/operator/aggregate/simple_aggregate.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "binder/expression/expression_util.h"
#include "common/data_chunk/data_chunk_state.h"
#include "common/in_mem_overflow_buffer.h"
#include "common/system_config.h"
#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "function/aggregate_function.h"
#include "main/client_context.h"
#include "processor/execution_context.h"
#include "processor/operator/aggregate/aggregate_hash_table.h"
#include "processor/operator/aggregate/aggregate_input.h"
#include "processor/operator/aggregate/base_aggregate.h"
#include "processor/result/factorized_table.h"
#include "processor/result/factorized_table_schema.h"
#include "storage/buffer_manager/memory_manager.h"

using namespace lbug::common;
using namespace lbug::function;

namespace lbug {
namespace processor {

std::string SimpleAggregatePrintInfo::toString() const {
    std::string result = "";
    result += "Aggregate: ";
    result += binder::ExpressionUtil::toString(aggregates);
    return result;
}

static bool isAnyFunctionDistinct(const std::vector<AggregateFunction>& functions) {
    return std::any_of(functions.begin(), functions.end(),
        [&](auto& func) { return func.isDistinct; });
}

SimpleAggregateSharedState::SimpleAggregateSharedState(main::ClientContext* context,
    const std::vector<AggregateFunction>& aggregateFunctions,
    const std::vector<AggregateInfo>& aggInfos)
    : BaseAggregateSharedState{aggregateFunctions,
          // Only distinct functions need partitioning
          getNumPartitionsForParallelism(context)},
      hasDistinct{isAnyFunctionDistinct(aggregateFunctions)},
      globalPartitions{hasDistinct ? getNumPartitionsForParallelism(context) : 0},
      aggregateOverflowBuffer{storage::MemoryManager::Get(*context)} {
    auto mm = storage::MemoryManager::Get(*context);
    for (size_t funcIdx = 0; funcIdx < this->aggregateFunctions.size(); funcIdx++) {
        auto& aggregateFunction = this->aggregateFunctions[funcIdx];
        globalAggregateStates.push_back(aggregateFunction.createInitialNullAggregateState());
        partitioningData.emplace_back(this, funcIdx);
        if (aggregateFunction.isDistinct) {
            auto schema = AggregateHashTableUtils::getTableSchemaForKeys(std::vector<LogicalType>{},
                aggInfos[funcIdx].distinctAggKeyType);
            for (auto& partition : globalPartitions) {
                partition.distinctTables.emplace_back(
                    Partition::DistinctData{std::make_unique<HashTableQueue>(mm, schema.copy()),
                        aggregateFunction.createInitialNullAggregateState()});
            }
        } else {
            for (auto& partition : globalPartitions) {
                partition.distinctTables.emplace_back();
            }
        }
    }
}

void SimpleAggregateSharedState::combineAggregateStates(
    const std::vector<std::unique_ptr<AggregateState>>& localAggregateStates,
    common::InMemOverflowBuffer&& localOverflowBuffer) {
    DASSERT(localAggregateStates.size() == globalAggregateStates.size());
    std::unique_lock lck{mtx};
    for (auto i = 0u; i < aggregateFunctions.size(); ++i) {
        // Distinct functions will be combined accross the partitions in
        // finalizeAggregateStates
        aggregateOverflowBuffer.merge(localOverflowBuffer);
        if (!aggregateFunctions[i].isDistinct) {
            aggregateFunctions[i].combineState(
                reinterpret_cast<uint8_t*>(globalAggregateStates[i].get()),
                reinterpret_cast<uint8_t*>(localAggregateStates[i].get()),
                &aggregateOverflowBuffer);
        }
    }
}

void SimpleAggregateSharedState::finalizeAggregateStates() {
    std::unique_lock lck{mtx};
    for (auto i = 0u; i < aggregateFunctions.size(); ++i) {
        if (aggregateFunctions[i].isDistinct) {
            for (auto& partition : globalPartitions) {
                aggregateFunctions[i].combineState(reinterpret_cast<uint8_t*>(getAggregateState(i)),
                    reinterpret_cast<uint8_t*>(partition.distinctTables[i].state.get()),
                    &aggregateOverflowBuffer);
            }
        }
        aggregateFunctions[i].finalizeState(
            reinterpret_cast<uint8_t*>(globalAggregateStates[i].get()));
    }
}

void SimpleAggregateSharedState::resetForReuse() {
    // A cached physical plan aliases this state across executions of the operator tree, so the
    // scan cursor and the accumulated aggregate states must be re-armed before each execution.
    std::unique_lock lck{mtx};
    currentOffset.store(0);
    readyForFinalization = false;
    std::unique_ptr<common::InMemOverflowBuffer> buf;
    while (overflow.pop(buf)) {}
    aggregateOverflowBuffer.resetBuffer();
    for (size_t i = 0; i < aggregateFunctions.size(); ++i) {
        globalAggregateStates[i] = aggregateFunctions[i].createInitialNullAggregateState();
    }
    if (!hasDistinct) {
        return;
    }
    // finalizePartitions folded the previous execution's distinct values into the partition
    // states; start them again from the initial state, and drop tuples left by an execution that
    // stopped before finalizing.
    for (auto& partition : globalPartitions) {
        for (size_t funcIdx = 0; funcIdx < aggregateFunctions.size(); funcIdx++) {
            auto& aggregateFunction = aggregateFunctions[funcIdx];
            if (!aggregateFunction.isDistinct) {
                continue;
            }
            auto& distinctData = partition.distinctTables[funcIdx];
            distinctData.queue->clear();
            distinctData.state = aggregateFunction.createInitialNullAggregateState();
        }
        partition.finalized = false;
    }
}

std::pair<uint64_t, uint64_t> SimpleAggregateSharedState::getNextRangeToRead() {
    std::unique_lock lck{mtx};
    if (currentOffset >= 1) {
        return std::make_pair(currentOffset.load(), currentOffset.load());
    }
    auto startOffset = currentOffset.load();
    currentOffset++;
    return std::make_pair(startOffset, currentOffset.load());
}

void SimpleAggregateSharedState::SimpleAggregatePartitioningData::appendTuples(
    const FactorizedTable& factorizedTable, ft_col_offset_t hashOffset) {
    DASSERT(sharedState->globalPartitions.size() > 0);
    auto numBytesPerTuple = factorizedTable.getTableSchema()->getNumBytesPerTuple();
    for (ft_tuple_idx_t tupleIdx = 0; tupleIdx < factorizedTable.getNumTuples(); tupleIdx++) {
        auto tuple = factorizedTable.getTuple(tupleIdx);
        // Tuples are packed without alignment padding; use memcpy for the hash load.
        common::hash_t hash;
        memcpy(&hash, tuple + hashOffset, sizeof(common::hash_t));
        // shiftForPartitioning is 64 with a single partition; shifting by 64 is UB.
        const auto partitionIdx =
            sharedState->shiftForPartitioning >= 64 ?
                0 :
                (hash >> sharedState->shiftForPartitioning) % sharedState->globalPartitions.size();
        auto& partition = sharedState->globalPartitions[partitionIdx];
        partition.distinctTables[functionIdx].queue->appendTuple(
            std::span(tuple, numBytesPerTuple));
    }
}

// LCOV_EXCL_START
void SimpleAggregateSharedState::SimpleAggregatePartitioningData::appendDistinctTuple(size_t,
    std::span<uint8_t>, common::hash_t) {
    UNREACHABLE_CODE;
}
// LCOV_EXCL_END

void SimpleAggregateSharedState::SimpleAggregatePartitioningData::appendOverflow(
    common::InMemOverflowBuffer&& overflowBuffer) {
    sharedState->overflow.push(
        std::make_unique<common::InMemOverflowBuffer>(std::move(overflowBuffer)));
}

void SimpleAggregateSharedState::finalizePartitions(storage::MemoryManager* memoryManager,
    const std::vector<AggregateInfo>& aggInfos) {
    if (!hasDistinct) {
        return;
    }
    InMemOverflowBuffer localOverflowBuffer(memoryManager);
    BaseAggregateSharedState::finalizePartitions(globalPartitions, [&](auto& partition) {
        for (size_t i = 0; i < partition.distinctTables.size(); i++) {
            if (!aggregateFunctions[i].isDistinct) {
                continue;
            }
            auto& [queue, state] = partition.distinctTables[i];
            // A partition whose queue received no values keeps its initial state.
            if (queue->empty()) {
                continue;
            }
            std::vector<LogicalType> keyTypes;
            keyTypes.push_back(aggInfos[i].distinctAggKeyType.copy());
            AggregateHashTable hashTable(*memoryManager, std::move(keyTypes),
                std::vector<LogicalType>{} /*payloadTypes*/, std::vector<AggregateFunction>{},
                std::vector<LogicalType>{}, 0,
                AggregateHashTableUtils::getTableSchemaForKeys(std::vector<LogicalType>{},
                    aggInfos[i].distinctAggKeyType));
            queue->mergeInto(hashTable);

            ValueVector aggregateVector(aggInfos[i].distinctAggKeyType.copy(), memoryManager,
                std::make_shared<DataChunkState>());
            const auto* ft = hashTable.getFactorizedTable();
            ft_tuple_idx_t startTupleIdx = 0;
            ft_tuple_idx_t numTuplesToScan =
                std::min(DEFAULT_VECTOR_CAPACITY, ft->getNumTuples() - startTupleIdx);
            std::array<uint32_t, 1> colIdxToScan = {0};
            std::array<ValueVector*, 1> vectors = {&aggregateVector};
            while (numTuplesToScan > 0) {
                ft->scan(vectors, startTupleIdx, numTuplesToScan, colIdxToScan);
                aggregateFunctions[i].updateAllState((uint8_t*)state.get(), &aggregateVector,
                    1 /*multiplicity*/, &localOverflowBuffer);
                startTupleIdx += numTuplesToScan;
                numTuplesToScan =
                    std::min(DEFAULT_VECTOR_CAPACITY, ft->getNumTuples() - startTupleIdx);
            }
        }
    });
    {
        std::unique_lock lck{mtx};
        aggregateOverflowBuffer.merge(localOverflowBuffer);
    }
}

void SimpleAggregate::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) {
    localAggregateStates.clear();
    distinctHashTables.clear();
    BaseAggregate::initLocalStateInternal(resultSet, context);
    for (auto& func : aggregateFunctions) {
        localAggregateStates.push_back(func.createInitialNullAggregateState());
        distinctHashTables.emplace_back();
    }
}

std::unique_ptr<PartitioningAggregateHashTable> SimpleAggregate::createDistinctHashTable(
    size_t funcIdx, storage::MemoryManager& memoryManager) {
    std::vector<LogicalType> keyTypes;
    keyTypes.push_back(aggInfos[funcIdx].distinctAggKeyType.copy());
    return std::make_unique<PartitioningAggregateHashTable>(
        &getSharedState().partitioningData[funcIdx], memoryManager, std::move(keyTypes),
        std::vector<LogicalType>{} /* empty payload*/, std::vector<function::AggregateFunction>{},
        std::vector<LogicalType>{} /*empty distinct keys*/,
        AggregateHashTableUtils::getTableSchemaForKeys(std::vector<LogicalType>{},
            aggInfos[funcIdx].distinctAggKeyType));
}

void SimpleAggregate::executeInternal(ExecutionContext* context) {
    auto* memoryManager = storage::MemoryManager::Get(*context->clientContext);
    InMemOverflowBuffer localOverflowBuffer(memoryManager);
    while (children[0]->getNextTuple(context)) {
        for (auto i = 0u; i < aggregateFunctions.size(); i++) {
            auto aggregateFunction = &aggregateFunctions[i];
            if (aggregateFunction->isFunctionDistinct()) {
                if (!distinctHashTables[i]) {
                    distinctHashTables[i] = createDistinctHashTable(i, *memoryManager);
                }
                // Just add distinct value to the hash table. We'll calculate the aggregate state
                // once it's been merged into the shared state
                distinctHashTables[i]->appendDistinct(std::vector<ValueVector*>{},
                    aggInputs[i].aggregateVector, aggInputs[i].aggregateVector->state.get());
            } else {
                computeAggregate(aggregateFunction, &aggInputs[i], localAggregateStates[i].get(),
                    localOverflowBuffer);
            }
        }
    }
    if (getSharedState().hasDistinct) {
        for (auto& hashTable : distinctHashTables) {
            if (hashTable) {
                hashTable->mergeIfFull(0 /*tuplesToAdd*/, true /*mergeAll*/);
            }
        }
    }
    getSharedState().combineAggregateStates(localAggregateStates, std::move(localOverflowBuffer));
}

void SimpleAggregate::computeAggregate(function::AggregateFunction* function, AggregateInput* input,
    function::AggregateState* state, common::InMemOverflowBuffer& overflowBuffer) {
    auto multiplicity = resultSet->multiplicity;
    for (auto dataChunk : input->multiplicityChunks) {
        multiplicity *= dataChunk->state->getSelVector().getSelSize();
    }
    if (input->aggregateVector && input->aggregateVector->state->isFlat()) {
        auto pos = input->aggregateVector->state->getSelVector()[0];
        if (!input->aggregateVector->isNull(pos)) {
            function->updatePosState((uint8_t*)state, input->aggregateVector, multiplicity, pos,
                &overflowBuffer);
        }
    } else {
        function->updateAllState((uint8_t*)state, input->aggregateVector, multiplicity,
            &overflowBuffer);
    }
}

void SimpleAggregateFinalize::finalizeInternal(ExecutionContext* /*context*/) {
    sharedState->finalizeAggregateStates();
    if (metrics) {
        metrics->numOutputTuple.incrementByOne();
    }
}

void SimpleAggregateFinalize::executeInternal(ExecutionContext* context) {
    DASSERT(sharedState->isReadyForFinalization());
    sharedState->finalizePartitions(storage::MemoryManager::Get(*context->clientContext), aggInfos);
}

} // namespace processor
} // namespace lbug

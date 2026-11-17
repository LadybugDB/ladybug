#pragma once

#include <mutex>

#include "aggregate_input.h"
#include "common/mpsc_queue.h"
#include "function/aggregate_function.h"
#include "processor/operator/sink.h"
#include "processor/result/factorized_table.h"
#include "processor/result/factorized_table_schema.h"

namespace lbug {
namespace main {
class ClientContext;
}
namespace processor {
class AggregateHashTable;

size_t getNumPartitionsForParallelism(main::ClientContext* context);

class BaseAggregateSharedState {
    friend class BaseAggregate;

public:
    template<typename Partition, typename Func>
    void finalizePartitions(std::vector<Partition>& globalPartitions, Func finalizeFunc) {
        for (auto& partition : globalPartitions) {
            if (!partition.finalized && partition.mtx.try_lock()) {
                if (partition.finalized) {
                    // If there was a data race in the above && a thread may get through after
                    // another thread has finalized this partition Ignore coverage since we can't
                    // reliably test this data race
                    // LCOV_EXCL_START
                    partition.mtx.unlock();
                    continue;
                    // LCOV_EXCL_END
                }
                finalizeFunc(partition);
                partition.finalized = true;
                partition.mtx.unlock();
            }
        }
    }

    bool isReadyForFinalization() const { return readyForFinalization; }

protected:
    explicit BaseAggregateSharedState(
        const std::vector<function::AggregateFunction>& aggregateFunctions, size_t numPartitions);

    virtual std::pair<uint64_t, uint64_t> getNextRangeToRead() = 0;

    ~BaseAggregateSharedState() = default;

    void finalizeAggregateHashTable(const AggregateHashTable& localHashTable);

    // There is one partition per configured thread, and most of them receive no tuples when there
    // are few groups, so a queue allocates its first block on the first appendTuple. mergeInto()
    // and clear() leave the queue empty, and it can take tuples again. appendTuple may run on
    // several threads at once; mergeInto, clear and empty only once all appends have finished.
    class HashTableQueue {
    public:
        HashTableQueue(storage::MemoryManager* memoryManager, FactorizedTableSchema tableSchema)
            : memoryManager{memoryManager}, tableSchema{std::move(tableSchema)},
              headBlock{nullptr} {}

        std::unique_ptr<HashTableQueue> copy() const {
            return std::make_unique<HashTableQueue>(memoryManager, tableSchema.copy());
        }
        ~HashTableQueue() { clear(); }

        void appendTuple(std::span<uint8_t> tuple);

        void mergeInto(AggregateHashTable& hashTable);

        void clear();

        bool empty() const {
            auto headBlock = this->headBlock.load();
            return (headBlock == nullptr || headBlock->numTuplesReserved == 0) &&
                   queuedTuples.approxSize() == 0;
        }

        struct TupleBlock {
            TupleBlock(storage::MemoryManager* memoryManager, FactorizedTableSchema tableSchema)
                : numTuplesReserved{0}, numTuplesWritten{0},
                  table{memoryManager, std::move(tableSchema)} {
                // Start at a fixed capacity of one full block (so that concurrent writes are safe).
                // If it is not filled, we resize it to the actual capacity before writing it to the
                // hashTable
                table.resize(table.getNumTuplesPerBlock());
            }
            // numTuplesReserved may be greater than the capacity of the factorizedTable
            // if threads try to write to it while a new block is being allocated
            // So it should not be relied on for anything other than reserving tuples
            std::atomic<uint64_t> numTuplesReserved;
            // Set after the tuple has been written to the block.
            // Once numTuplesWritten == factorizedTable.getNumTuplesPerBlock() all writes have
            // finished
            std::atomic<uint64_t> numTuplesWritten;
            FactorizedTable table;
        };
        storage::MemoryManager* memoryManager;
        FactorizedTableSchema tableSchema;
        common::MPSCQueue<TupleBlock*> queuedTuples;
        // When queueing tuples, they are always added to the headBlock until the headBlock is full
        // (numTuplesReserved >= factorizedTable.getNumTuplesPerBlock()), then pushed into the
        // queuedTuples (at which point, the numTuplesReserved may not be equal to the
        // numTuplesWritten). nullptr until the first tuple and after mergeInto() or clear().
        std::atomic<TupleBlock*> headBlock;
        // Serializes installing the first block, so that threads flushing into an empty queue at
        // the same time don't each allocate one.
        std::mutex firstBlockMtx;
    };

protected:
    std::mutex mtx;
    std::atomic<uint64_t> currentOffset;
    std::vector<function::AggregateFunction> aggregateFunctions;
    std::atomic<size_t> numThreadsFinishedProducing;
    std::atomic<size_t> numThreads;
    common::MPSCQueue<std::unique_ptr<common::InMemOverflowBuffer>> overflow;
    uint8_t shiftForPartitioning;
    bool readyForFinalization;
};

class BaseAggregate : public Sink {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::AGGREGATE;

protected:
    BaseAggregate(std::shared_ptr<BaseAggregateSharedState> sharedState,
        std::vector<function::AggregateFunction> aggregateFunctions,
        std::vector<AggregateInfo> aggInfos, std::unique_ptr<PhysicalOperator> child, uint32_t id,
        std::unique_ptr<OPPrintInfo> printInfo)
        : Sink{type_, std::move(child), id, std::move(printInfo)},
          aggregateFunctions{std::move(aggregateFunctions)}, aggInfos{std::move(aggInfos)},
          sharedState{std::move(sharedState)} {}

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;

    bool containDistinctAggregate() const;

    void finalizeInternal(ExecutionContext* /*context*/) override {
        // Delegated to HashAggregateFinalize so it can be parallelized
        sharedState->readyForFinalization = true;
    }

    std::unique_ptr<PhysicalOperator> copy() override = 0;

protected:
    std::vector<function::AggregateFunction> aggregateFunctions;
    std::vector<AggregateInfo> aggInfos;
    std::vector<AggregateInput> aggInputs;
    std::shared_ptr<BaseAggregateSharedState> sharedState;
};

} // namespace processor
} // namespace lbug

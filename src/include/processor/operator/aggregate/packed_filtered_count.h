#pragma once

#include <mutex>
#include <unordered_map>

#include "binder/expression/expression.h"
#include "processor/data_pos.h"
#include "processor/operator/sink.h"

namespace lbug {
namespace processor {

struct PackedFilteredCountInfo {
    DataPos groupKeyInputPos;
    DataPos groupKeyOutputPos;
    DataPos countOutputPos;
    DataPos lhsValuePos;
    DataPos rhsValuePos;
    data_chunk_pos_t selectChunkPos;
    data_chunk_pos_t flatChunkPos;
    std::vector<data_chunk_pos_t> multiplicityChunkPos;

    PackedFilteredCountInfo(DataPos groupKeyInputPos, DataPos groupKeyOutputPos,
        DataPos countOutputPos, DataPos lhsValuePos, DataPos rhsValuePos,
        data_chunk_pos_t selectChunkPos, data_chunk_pos_t flatChunkPos,
        std::vector<data_chunk_pos_t> multiplicityChunkPos)
        : groupKeyInputPos{groupKeyInputPos}, groupKeyOutputPos{groupKeyOutputPos},
          countOutputPos{countOutputPos}, lhsValuePos{lhsValuePos}, rhsValuePos{rhsValuePos},
          selectChunkPos{selectChunkPos}, flatChunkPos{flatChunkPos},
          multiplicityChunkPos{std::move(multiplicityChunkPos)} {}
};

struct PackedFilteredCountSharedState {
    std::mutex mtx;
    std::unordered_map<int64_t, uint64_t> counts;
    std::vector<std::pair<int64_t, uint64_t>> finalizedCounts;
    common::offset_t nextOffset = 0;
    bool finalized = false;

    void merge(std::unordered_map<int64_t, uint64_t>&& localCounts);
    void finalize();
    std::pair<common::offset_t, common::offset_t> getNextRangeToRead();
};

struct PackedFilteredCountPrintInfo final : OPPrintInfo {
    std::shared_ptr<binder::Expression> predicate;
    binder::expression_vector keys;

    PackedFilteredCountPrintInfo(std::shared_ptr<binder::Expression> predicate,
        binder::expression_vector keys)
        : predicate{std::move(predicate)}, keys{std::move(keys)} {}

    std::string toString() const override;

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::unique_ptr<PackedFilteredCountPrintInfo>(
            new PackedFilteredCountPrintInfo(*this));
    }

private:
    PackedFilteredCountPrintInfo(const PackedFilteredCountPrintInfo& other)
        : OPPrintInfo{other}, predicate{other.predicate}, keys{other.keys} {}
};

class PackedFilteredCount final : public Sink {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::PACKED_FILTERED_COUNT;

public:
    PackedFilteredCount(std::shared_ptr<PackedFilteredCountSharedState> sharedState,
        PackedFilteredCountInfo info, std::unique_ptr<PhysicalOperator> child, physical_op_id id,
        std::unique_ptr<OPPrintInfo> printInfo)
        : Sink{type_, std::move(child), id, std::move(printInfo)},
          sharedState{std::move(sharedState)}, info{std::move(info)} {}

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;

    void executeInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<PackedFilteredCount>(sharedState, info, children[0]->copy(), id,
            printInfo->copy());
    }

private:
    uint64_t countMatchesForCurrentTuple();

private:
    std::shared_ptr<PackedFilteredCountSharedState> sharedState;
    PackedFilteredCountInfo info;
    common::ValueVector* groupKeyVector = nullptr;
    common::ValueVector* lhsValueVector = nullptr;
    common::ValueVector* rhsValueVector = nullptr;
    common::DataChunkState* selectState = nullptr;
    common::DataChunkState* flatState = nullptr;
    std::vector<common::DataChunkState*> multiplicityStates;
    std::unordered_map<int64_t, uint64_t> localCounts;
};

class PackedFilteredCountScan final : public PhysicalOperator {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::AGGREGATE_SCAN;

public:
    PackedFilteredCountScan(std::shared_ptr<PackedFilteredCountSharedState> sharedState,
        DataPos groupKeyOutputPos, DataPos countOutputPos, std::unique_ptr<PhysicalOperator> child,
        physical_op_id id, std::unique_ptr<OPPrintInfo> printInfo)
        : PhysicalOperator{type_, std::move(child), id, std::move(printInfo)},
          sharedState{std::move(sharedState)}, groupKeyOutputPos{groupKeyOutputPos},
          countOutputPos{countOutputPos} {}

    bool isSource() const override { return true; }

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;

    bool getNextTuplesInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<PackedFilteredCountScan>(sharedState, groupKeyOutputPos,
            countOutputPos, children[0]->copy(), id, printInfo->copy());
    }

private:
    std::shared_ptr<PackedFilteredCountSharedState> sharedState;
    DataPos groupKeyOutputPos;
    DataPos countOutputPos;
    common::ValueVector* groupKeyVector = nullptr;
    common::ValueVector* countVector = nullptr;
};

} // namespace processor
} // namespace lbug

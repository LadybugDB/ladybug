#pragma once

#include "processor/operator/aggregate/base_aggregate_scan.h"
#include "processor/operator/aggregate/hash_aggregate.h"

namespace lbug {
namespace processor {

class HashAggregateScan final : public BaseAggregateScan {
public:
    HashAggregateScan(std::shared_ptr<HashAggregateSharedState> sharedState,
        std::vector<DataPos> groupByKeyVectorsPos, AggregateScanInfo scanInfo, uint32_t id,
        std::unique_ptr<OPPrintInfo> printInfo)
        : BaseAggregateScan{std::move(scanInfo), id, std::move(printInfo)},
          groupByKeyVectorsPos{std::move(groupByKeyVectorsPos)},
          sharedState{std::move(sharedState)} {}

    HashAggregateScan(std::shared_ptr<HashAggregateSharedState> sharedState,
        std::vector<DataPos> groupByKeyVectorsPos, AggregateScanInfo scanInfo,
        std::unique_ptr<PhysicalOperator> child, uint32_t id,
        std::unique_ptr<OPPrintInfo> printInfo)
        : BaseAggregateScan{std::move(scanInfo), std::move(child), id, std::move(printInfo)},
          groupByKeyVectorsPos{std::move(groupByKeyVectorsPos)},
          sharedState{std::move(sharedState)} {}

    std::shared_ptr<HashAggregateSharedState> getSharedState() const { return sharedState; }

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;

    bool getNextTuplesInternal(ExecutionContext* context) override;

    // Note: The child must be preserved. PlanMapper attaches the aggregate's finalize pipeline
    // below this scan, and physical-plan caching relies on copy() producing a complete tree;
    // dropping the child here would silently remove whole pipelines from cloned plans.
    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<HashAggregateScan>(sharedState, groupByKeyVectorsPos,
            AggregateScanInfo{scanInfo.aggregatesPos, scanInfo.moveAggResultToVectorFuncs},
            children[0]->copy(), id, printInfo->copy());
    }

    double getProgress(ExecutionContext* context) const override;

private:
    std::vector<DataPos> groupByKeyVectorsPos;
    std::vector<common::ValueVector*> groupByKeyVectors;
    std::shared_ptr<HashAggregateSharedState> sharedState;
    std::vector<uint32_t> groupByKeyVectorsColIdxes;
    std::vector<uint8_t*> entries;
};

} // namespace processor
} // namespace lbug

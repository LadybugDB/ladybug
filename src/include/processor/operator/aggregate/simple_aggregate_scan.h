#pragma once

#include "processor/operator/aggregate/base_aggregate_scan.h"
#include "processor/operator/aggregate/simple_aggregate.h"

namespace lbug {
namespace processor {

class SimpleAggregateScan final : public BaseAggregateScan {
public:
    SimpleAggregateScan(std::shared_ptr<SimpleAggregateSharedState> sharedState,
        AggregateScanInfo scanInfo, uint32_t id, std::unique_ptr<OPPrintInfo> printInfo)
        : BaseAggregateScan{std::move(scanInfo), id, std::move(printInfo)},
          sharedState{std::move(sharedState)}, outDataChunk{nullptr} {}

    SimpleAggregateScan(std::shared_ptr<SimpleAggregateSharedState> sharedState,
        AggregateScanInfo scanInfo, std::unique_ptr<PhysicalOperator> child, uint32_t id,
        std::unique_ptr<OPPrintInfo> printInfo)
        : BaseAggregateScan{std::move(scanInfo), std::move(child), id, std::move(printInfo)},
          sharedState{std::move(sharedState)}, outDataChunk{nullptr} {}

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;

    bool getNextTuplesInternal(ExecutionContext* context) override;

    // Note: The child must be preserved. PlanMapper attaches the aggregate's finalize pipeline
    // below this scan, and physical-plan caching relies on copy() producing a complete tree;
    // dropping the child here would silently remove whole pipelines from cloned plans.
    std::unique_ptr<PhysicalOperator> copy() override {
        return make_unique<SimpleAggregateScan>(sharedState,
            AggregateScanInfo{scanInfo.aggregatesPos, scanInfo.moveAggResultToVectorFuncs},
            children[0]->copy(), id, printInfo->copy());
    }

private:
    std::shared_ptr<SimpleAggregateSharedState> sharedState;
    common::DataChunk* outDataChunk;
};

} // namespace processor
} // namespace lbug

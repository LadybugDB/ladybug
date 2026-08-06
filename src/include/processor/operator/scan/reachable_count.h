#pragma once

#include "graph/graph_entry.h"
#include "graph/on_disk_graph.h"
#include "processor/operator/physical_operator.h"
#include "storage/table/rel_table.h"

namespace lbug {
namespace processor {

struct ReachableCountPrintInfo final : OPPrintInfo {
    std::string relTableName;
    uint16_t lowerBound;
    uint16_t upperBound;

    ReachableCountPrintInfo(std::string relTableName, uint16_t lowerBound, uint16_t upperBound)
        : relTableName{std::move(relTableName)}, lowerBound{lowerBound}, upperBound{upperBound} {}

    std::string toString() const override {
        return "Table: " + relTableName + ", Bounds: [" + std::to_string(lowerBound) + ".." +
               std::to_string(upperBound) + "]";
    }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<ReachableCountPrintInfo>(relTableName, lowerBound, upperBound);
    }
};

/**
 * PhysicalReachableCount computes COUNT(DISTINCT nbr) for a variable-length path starting from a
 * fixed set of source node offsets. It performs a bounded breadth-first traversal over the rel
 * table(s), counting every distinct neighbor node reached by a walk of any length in
 * [lowerBound, upperBound].
 */
class ReachableCount final : public PhysicalOperator {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::REACHABLE_COUNT;

public:
    ReachableCount(graph::NativeGraphEntry graphEntry, common::RelDataDirection direction,
        uint16_t lowerBound, uint16_t upperBound, common::table_id_t boundTableID,
        common::table_id_t nbrTableID, std::vector<common::offset_t> startOffsets,
        DataPos countOutputPos, physical_op_id id, std::unique_ptr<OPPrintInfo> printInfo)
        : PhysicalOperator{type_, id, std::move(printInfo)}, graphEntry{std::move(graphEntry)},
          direction{direction}, lowerBound{lowerBound}, upperBound{upperBound},
          boundTableID{boundTableID}, nbrTableID{nbrTableID}, startOffsets{std::move(startOffsets)},
          countOutputPos{countOutputPos} {}

    bool isSource() const override { return true; }
    bool isParallel() const override { return false; }

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;
    bool getNextTuplesInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<ReachableCount>(graphEntry.copy(), direction, lowerBound,
            upperBound, boundTableID, nbrTableID, startOffsets, countOutputPos, id,
            printInfo->copy());
    }

private:
    common::offset_t computeReachableCount();

private:
    graph::NativeGraphEntry graphEntry;
    common::RelDataDirection direction;
    uint16_t lowerBound;
    uint16_t upperBound;
    common::table_id_t boundTableID;
    common::table_id_t nbrTableID;
    std::vector<common::offset_t> startOffsets;
    DataPos countOutputPos;

    common::ValueVector* countVector = nullptr;
    std::unique_ptr<graph::OnDiskGraph> graph;
    std::vector<graph::GraphRelInfo> relInfos;
    std::vector<std::unique_ptr<graph::NbrScanState>> scanStates;
    bool hasExecuted = false;
};

} // namespace processor
} // namespace lbug

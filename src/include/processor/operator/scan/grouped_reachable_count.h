#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/data_chunk/data_chunk_state.h"
#include "common/types/types.h"
#include "graph/graph_entry.h"
#include "graph/on_disk_graph.h"
#include "processor/data_pos.h"
#include "processor/operator/physical_operator.h"
#include "storage/table/node_table.h"

namespace lbug {
namespace processor {

struct GroupedReachableCountPrintInfo final : OPPrintInfo {
    std::string relTableName;
    uint16_t lowerBound;
    uint16_t upperBound;
    size_t numKeys;
    bool hasCount;
    bool hasAvg;

    GroupedReachableCountPrintInfo(std::string relTableName, uint16_t lowerBound,
        uint16_t upperBound, size_t numKeys, bool hasCount, bool hasAvg)
        : relTableName{std::move(relTableName)}, lowerBound{lowerBound}, upperBound{upperBound},
          numKeys{numKeys}, hasCount{hasCount}, hasAvg{hasAvg} {}

    std::string toString() const override {
        return "Table: " + relTableName + ", Bounds: [" + std::to_string(lowerBound) + ".." +
               std::to_string(upperBound) + "], Keys: " + std::to_string(numKeys) +
               (hasCount ? ", COUNT" : "") + (hasAvg ? ", AVG" : "");
    }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<GroupedReachableCountPrintInfo>(relTableName, lowerBound,
            upperBound, numKeys, hasCount, hasAvg);
    }
};

/**
 * PhysicalGroupedReachableCount answers GROUP BY b.keys, COUNT(*), AVG(b.score) over a
 * variable-length path (a)-[r*lo..up]->(b) sourced from the full bound table, without
 * materializing the (a, b) walks.
 *
 * Execution:
 *  1. Per-destination walk counts via level DP over CSR: L_0[v] = 1 for every visible source
 *     node, L_{d+1}[v] = sum of L_d[u] over incoming edges (u -> v); the walk count of `v` is
 *     sum_{d in [lo, up]} L_d[v]. Time O(up * E), memory O(V).
 *  2. One point lookup of the group keys + AVG input per destination node with a non-zero
 *     walk count, folded into per-group (count, sum) accumulators. COUNT and AVG weight
 *     every walk, matching row-wise aggregation over the unoptimized plan.
 */
class GroupedReachableCount final : public PhysicalOperator {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::GROUPED_REACHABLE_COUNT;

public:
    struct KeyInfo {
        // Storage column of the group key, or INVALID_COLUMN_ID when the key is the node's
        // internal ID (its value is the node offset itself, no column read needed).
        common::column_id_t columnID = common::INVALID_COLUMN_ID;
        common::LogicalType logicalType;
        bool isInternalID = false;
    };

    GroupedReachableCount(graph::NativeGraphEntry graphEntry, common::RelDataDirection direction,
        uint16_t lowerBound, uint16_t upperBound, common::table_id_t srcTableID,
        common::table_id_t dstTableID, storage::NodeTable* nodeTable, std::vector<KeyInfo> keyInfos,
        common::column_id_t scoreColumnID, common::LogicalType scoreType,
        std::vector<DataPos> keyOutputPoss, DataPos countOutputPos, bool hasCount,
        DataPos avgOutputPos, bool hasAvg, physical_op_id id,
        std::unique_ptr<OPPrintInfo> printInfo)
        : PhysicalOperator{type_, id, std::move(printInfo)}, graphEntry{std::move(graphEntry)},
          direction{direction}, lowerBound{lowerBound}, upperBound{upperBound},
          srcTableID{srcTableID}, dstTableID{dstTableID}, nodeTable{nodeTable},
          keyInfos{std::move(keyInfos)}, scoreColumnID{scoreColumnID},
          scoreType{std::move(scoreType)}, keyOutputPoss{std::move(keyOutputPoss)},
          countOutputPos{std::move(countOutputPos)}, hasCount{hasCount},
          avgOutputPos{std::move(avgOutputPos)}, hasAvg{hasAvg} {}

    bool isSource() const override { return true; }
    bool isParallel() const override { return false; }

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;
    bool getNextTuplesInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        std::vector<KeyInfo> keyInfosCopy;
        keyInfosCopy.reserve(keyInfos.size());
        for (auto& info : keyInfos) {
            keyInfosCopy.push_back(
                KeyInfo{info.columnID, info.logicalType.copy(), info.isInternalID});
        }
        return std::make_unique<GroupedReachableCount>(graphEntry.copy(), direction, lowerBound,
            upperBound, srcTableID, dstTableID, nodeTable, std::move(keyInfosCopy), scoreColumnID,
            scoreType.copy(), keyOutputPoss, countOutputPos, hasCount, avgOutputPos, hasAvg, id,
            printInfo->copy());
    }

private:
    struct KeyValue {
        common::LogicalType type;
        bool isNull = true;
        int64_t intVal = 0;
        double dblVal = 0.0;
        std::string strVal;
        common::nodeID_t nodeID = {common::INVALID_OFFSET, common::INVALID_TABLE_ID};
    };
    struct GroupAccumulator {
        std::vector<KeyValue> keys;
        uint64_t count = 0;
        double avgSum = 0.0;
        uint64_t avgCount = 0;
    };

    static void appendToKey(std::string& out, const KeyValue& key,
        common::PhysicalTypeID physicalType);
    void compute(ExecutionContext* context);
    void readNodeProperties(transaction::Transaction* transaction, common::offset_t offset,
        std::vector<KeyValue>& keys, bool& scoreIsNull, double& score);
    void emitRow(uint32_t pos, const GroupAccumulator& group);

private:
    graph::NativeGraphEntry graphEntry;
    common::RelDataDirection direction;
    uint16_t lowerBound;
    uint16_t upperBound;
    common::table_id_t srcTableID;
    common::table_id_t dstTableID;
    storage::NodeTable* nodeTable;
    std::vector<KeyInfo> keyInfos;
    common::column_id_t scoreColumnID;
    common::LogicalType scoreType;
    std::vector<DataPos> keyOutputPoss;
    DataPos countOutputPos;
    bool hasCount;
    DataPos avgOutputPos;
    bool hasAvg;

    std::vector<common::ValueVector*> keyVectors;
    common::ValueVector* countVector = nullptr;
    common::ValueVector* avgVector = nullptr;
    std::unique_ptr<graph::OnDiskGraph> graph;
    std::vector<graph::GraphRelInfo> relInfos;
    std::vector<std::unique_ptr<graph::NbrScanState>> scanStates;

    std::vector<GroupAccumulator> groups;
    size_t emitIdx = 0;
    bool computed = false;
};

} // namespace processor
} // namespace lbug

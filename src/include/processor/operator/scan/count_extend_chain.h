#pragma once

#include <unordered_map>

#include "common/enums/rel_direction.h"
#include "common/types/types.h"
#include "processor/data_pos.h"
#include "processor/operator/physical_operator.h"
#include "storage/table/node_table.h"
#include "storage/table/rel_table.h"

namespace lbug {
namespace storage {
class MemoryManager;
}

namespace processor {

struct CountExtendChainPrintInfo final : OPPrintInfo {
    std::vector<std::string> relTableNames;
    uint64_t numHops;

    CountExtendChainPrintInfo(std::vector<std::string> relTableNames, uint64_t numHops)
        : relTableNames{std::move(relTableNames)}, numHops{numHops} {}

    std::string toString() const override {
        auto result = std::string("Tables: ");
        for (auto i = 0u; i < relTableNames.size(); ++i) {
            result += relTableNames[i];
            if (i + 1 < relTableNames.size()) {
                result += ", ";
            }
        }
        result += ", Hops: " + std::to_string(numHops);
        return result;
    }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<CountExtendChainPrintInfo>(relTableNames, numHops);
    }
};

/**
 * CountExtendChain computes COUNT(*) over a chain of rel-table extends using count-only
 * arithmetic. Instead of materializing the (multi-million-row) intermediate extend outputs and
 * hash joining them, each hop propagates a per-node int64 count vector:
 *
 *   c_0[v] = 1 for every node of the chain's source node table
 *   c_{j+1}[w] = sum over visible edges (v -> w) of rel table j of c_j[v]
 *   result     = sum over visible edges of the last hop of c_{N-1}[v]
 *
 * The per-hop edge scans reuse the standard rel table scan machinery (persistent CSR, in-memory
 * CSR, local storage and version/deletion filtering all come for free); the neighbor IDs read
 * from the neighbor column are consumed immediately by the accumulation and never materialized
 * as tuples. The operator is a single-threaded source emitting one row with the final count.
 */
class CountExtendChain final : public PhysicalOperator {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::COUNT_EXTEND_CHAIN;

public:
    struct Hop {
        std::vector<storage::RelTable*> relTables;
        std::vector<common::RelDataDirection> scanDirections;
        std::vector<common::table_id_t> fromNodeTableIDs;
        std::vector<common::table_id_t> toNodeTableIDs;
    };

    CountExtendChain(std::vector<Hop> hops,
        std::unordered_map<common::table_id_t, storage::NodeTable*> nodeTables,
        DataPos countOutputPos, physical_op_id id, std::unique_ptr<OPPrintInfo> printInfo)
        : PhysicalOperator{type_, id, std::move(printInfo)}, hops{std::move(hops)},
          nodeTables{std::move(nodeTables)}, countOutputPos{countOutputPos} {}

    bool isSource() const override { return true; }
    bool isParallel() const override { return false; }

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;

    bool getNextTuplesInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<CountExtendChain>(hops, nodeTables, countOutputPos, id,
            printInfo->copy());
    }

private:
    // Compute the upper bound (exclusive) of the node offsets that need to be iterated for the
    // given node table. This covers the full committed offset space (including in-memory node
    // groups), while c vectors are sized to numGroups * NODE_GROUP_SIZE so that offsets
    // referencing nodes of a partially-deleted tail group remain in bounds.
    common::offset_t getOffsetUpperBound(storage::NodeTable* nodeTable) const;

    void processBatch(uint64_t hopIdx, common::table_id_t fromTableID, common::table_id_t toTableID,
        bool isLastHop, const storage::RelTableScanState& scanState,
        const common::ValueVector& nodeIDVector, const common::ValueVector& nbrVector);

private:
    std::vector<Hop> hops;
    std::unordered_map<common::table_id_t, storage::NodeTable*> nodeTables;
    DataPos countOutputPos;
    common::ValueVector* countVector = nullptr;
    bool hasExecuted = false;
    int64_t totalCount = 0;
    // counts[j] maps a node table ID to the per-node partial path counts at chain position j.
    std::vector<std::unordered_map<common::table_id_t, std::vector<int64_t>>> counts;
};

} // namespace processor
} // namespace lbug

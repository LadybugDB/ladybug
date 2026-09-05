#pragma once

#include <unordered_map>

#include "common/enums/extend_direction.h"
#include "common/enums/rel_direction.h"
#include "common/system_config.h"
#include "common/types/types.h"
#include "processor/data_pos.h"
#include "processor/operator/physical_operator.h"
#include "storage/table/node_table.h"
#include "storage/table/rel_table.h"

namespace lbug {
namespace processor {

struct CountAntiEdgeChainPrintInfo final : OPPrintInfo {
    std::string relTableName;
    uint64_t numSuffixHops;

    CountAntiEdgeChainPrintInfo(std::string relTableName, uint64_t numSuffixHops)
        : relTableName{std::move(relTableName)}, numSuffixHops{numSuffixHops} {}

    std::string toString() const override {
        return "Anti-edge rel: " + relTableName + ", suffix hops: " + std::to_string(numSuffixHops);
    }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<CountAntiEdgeChainPrintInfo>(relTableName, numSuffixHops);
    }
};

/**
 * CountAntiEdgeChain computes COUNT(*) over LSQB-q9-shaped queries with pure count arithmetic.
 * See LogicalCountAntiEdgeChain for the pattern and the T - A - S formula. No hash joins are
 * performed and no tuples are materialized: the operator scans the anti-edge rel table to build
 * directional adjacency lists, scans the suffix rel tables backward to propagate per-node
 * suffix path counts D, then evaluates T, A and S over flat arrays.
 *
 * The two chain hops enumerate rows in chainN0Dir/chainN2Dir and the anti-edge match in
 * antiEdgeDir (each FWD, BWD or BOTH); the arithmetic models exactly that enumeration.
 *
 * Single-threaded source emitting one row with the final count (int64).
 */
class CountAntiEdgeChain final : public PhysicalOperator {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::COUNT_ANTI_EDGE_CHAIN;

public:
    struct Hop {
        std::vector<storage::RelTable*> relTables;
        std::vector<common::RelDataDirection> scanDirections;
        std::vector<storage::NodeTable*> fromNodeTables;
        std::vector<storage::NodeTable*> toNodeTables;
    };

    CountAntiEdgeChain(std::vector<Hop> suffixHops, storage::RelTable* antiRelTable,
        storage::NodeTable* midNodeTable, common::ExtendDirection chainN0Dir,
        common::ExtendDirection chainN2Dir, common::ExtendDirection antiEdgeDir, bool hasNotEquals,
        DataPos countOutputPos, physical_op_id id, std::unique_ptr<OPPrintInfo> printInfo)
        : PhysicalOperator{type_, id, std::move(printInfo)}, suffixHops{std::move(suffixHops)},
          antiRelTable{antiRelTable}, midNodeTable{midNodeTable}, chainN0Dir{chainN0Dir},
          chainN2Dir{chainN2Dir}, antiEdgeDir{antiEdgeDir}, hasNotEquals{hasNotEquals},
          countOutputPos{countOutputPos} {}

    bool isSource() const override { return true; }
    bool isParallel() const override { return false; }

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;

    bool getNextTuplesInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<CountAntiEdgeChain>(suffixHops, antiRelTable, midNodeTable,
            chainN0Dir, chainN2Dir, antiEdgeDir, hasNotEquals, countOutputPos, id,
            printInfo->copy());
    }

private:
    common::offset_t getOffsetUpperBound(storage::NodeTable* nodeTable) const {
        const auto numGroups = nodeTable->getNumNodeGroups();
        if (numGroups == 0) {
            return 0;
        }
        return (numGroups - 1) * common::StorageConfig::NODE_GROUP_SIZE +
               nodeTable->getNumTuplesInNodeGroup(numGroups - 1);
    }

    // Scan one rel table in the given direction with sequential bound-node batches, invoking
    // the per-batch callback with (scanState, nodeIDVector, nbrVector).
    template<typename Func>
    void scanRelRows(storage::RelTable* relTable, common::RelDataDirection direction,
        storage::NodeTable* boundNodeTable, transaction::Transaction* transaction,
        storage::MemoryManager* memoryManager, Func&& callback);

    // Backward suffix propagation: returns B[0], the per-node suffix path counts over the mid
    // node table.
    std::vector<int64_t> computeSuffixCounts(transaction::Transaction* transaction,
        storage::MemoryManager* memoryManager);

private:
    std::vector<Hop> suffixHops;
    storage::RelTable* antiRelTable;
    storage::NodeTable* midNodeTable;
    common::ExtendDirection chainN0Dir;
    common::ExtendDirection chainN2Dir;
    common::ExtendDirection antiEdgeDir;
    bool hasNotEquals;
    DataPos countOutputPos;
    common::ValueVector* countVector = nullptr;
    bool hasExecuted = false;
};

} // namespace processor
} // namespace lbug

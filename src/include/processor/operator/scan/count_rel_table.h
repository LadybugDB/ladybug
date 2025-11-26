#pragma once

#include "common/enums/rel_direction.h"
#include "processor/operator/physical_operator.h"
#include "storage/table/rel_table.h"

namespace lbug {
namespace processor {

struct CountRelTablePrintInfo final : OPPrintInfo {
    std::string relTableName;

    explicit CountRelTablePrintInfo(std::string relTableName)
        : relTableName{std::move(relTableName)} {}

    std::string toString() const override { return "Table: " + relTableName; }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<CountRelTablePrintInfo>(relTableName);
    }
};

/**
 * CountRelTable is a physical operator that counts the rows in a rel table
 * by scanning through bound nodes and counting edges.
 * It has a SCAN_NODE_TABLE child that provides the bound node IDs.
 */
class CountRelTable final : public PhysicalOperator {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::COUNT_REL_TABLE;

public:
    CountRelTable(std::vector<storage::RelTable*> relTables, common::RelDataDirection direction,
        DataPos nodeIDPos, DataPos countOutputPos, std::unique_ptr<PhysicalOperator> child,
        physical_op_id id, std::unique_ptr<OPPrintInfo> printInfo)
        : PhysicalOperator{type_, std::move(child), id, std::move(printInfo)},
          relTables{std::move(relTables)}, direction{direction}, nodeIDPos{nodeIDPos},
          countOutputPos{countOutputPos} {}

    bool isSource() const override { return false; }
    bool isParallel() const override { return false; }

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;

    bool getNextTuplesInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<CountRelTable>(relTables, direction, nodeIDPos, countOutputPos,
            children[0]->copy(), id, printInfo->copy());
    }

private:
    std::vector<storage::RelTable*> relTables;
    common::RelDataDirection direction;
    DataPos nodeIDPos;
    DataPos countOutputPos;
    common::ValueVector* nodeIDVector;
    common::ValueVector* countVector;
    std::unique_ptr<storage::RelTableScanState> scanState;
    // Dedicated output state for rel table scanning (separate from nodeIDVector's state)
    std::shared_ptr<common::DataChunkState> relScanOutState;
    bool hasExecuted;
    common::row_idx_t totalCount;
};

} // namespace processor
} // namespace lbug

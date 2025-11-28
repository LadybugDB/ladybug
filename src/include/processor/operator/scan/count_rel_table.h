#pragma once

#include "common/enums/rel_direction.h"
#include "processor/operator/physical_operator.h"
#include "storage/table/node_table.h"
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
 * CountRelTable is a source operator that counts edges in a rel table
 * by scanning through all bound nodes and counting their edges.
 * It creates its own internal vectors for node scanning (not exposed in ResultSet).
 */
class CountRelTable final : public PhysicalOperator {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::COUNT_REL_TABLE;

public:
    CountRelTable(std::vector<storage::NodeTable*> nodeTables,
        std::vector<storage::RelTable*> relTables, common::RelDataDirection direction,
        DataPos countOutputPos, physical_op_id id, std::unique_ptr<OPPrintInfo> printInfo)
        : PhysicalOperator{type_, id, std::move(printInfo)}, nodeTables{std::move(nodeTables)},
          relTables{std::move(relTables)}, direction{direction}, countOutputPos{countOutputPos} {}

    bool isSource() const override { return true; }
    bool isParallel() const override { return false; }

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;

    bool getNextTuplesInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<CountRelTable>(nodeTables, relTables, direction, countOutputPos, id,
            printInfo->copy());
    }

private:
    std::vector<storage::NodeTable*> nodeTables;
    std::vector<storage::RelTable*> relTables;
    common::RelDataDirection direction;
    DataPos countOutputPos;
    common::ValueVector* countVector;
    bool hasExecuted;
    common::row_idx_t totalCount;
};

} // namespace processor
} // namespace lbug

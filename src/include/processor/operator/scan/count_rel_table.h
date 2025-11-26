#pragma once

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
 * CountRelTable is a physical operator that directly returns the count of rows
 * in a rel table without scanning. It uses the table's getNumTotalRows() method.
 */
class CountRelTable final : public PhysicalOperator {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::COUNT_REL_TABLE;

public:
    CountRelTable(std::vector<storage::RelTable*> relTables, DataPos countOutputPos,
        physical_op_id id, std::unique_ptr<OPPrintInfo> printInfo)
        : PhysicalOperator{type_, id, std::move(printInfo)}, relTables{std::move(relTables)},
          countOutputPos{countOutputPos}, hasExecuted{false} {}

    bool isSource() const override { return true; }
    bool isParallel() const override { return false; }

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;

    bool getNextTuplesInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<CountRelTable>(relTables, countOutputPos, id, printInfo->copy());
    }

private:
    std::vector<storage::RelTable*> relTables;
    DataPos countOutputPos;
    common::ValueVector* countVector;
    bool hasExecuted;
};

} // namespace processor
} // namespace lbug

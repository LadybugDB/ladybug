#pragma once

#include "common/enums/rel_direction.h"
#include "planner/operator/scan/logical_rel_degree_table.h"
#include "processor/operator/physical_operator.h"
#include "storage/table/rel_table.h"

namespace lbug {
namespace processor {

struct RelDegreeTablePrintInfo final : OPPrintInfo {
    std::string relTableName;
    planner::RelDegreeTableMode mode;

    RelDegreeTablePrintInfo(std::string relTableName, planner::RelDegreeTableMode mode)
        : relTableName{std::move(relTableName)}, mode{mode} {}

    std::string toString() const override { return "Table: " + relTableName; }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<RelDegreeTablePrintInfo>(relTableName, mode);
    }
};

class RelDegreeTable final : public PhysicalOperator {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::REL_DEGREE_TABLE;

public:
    RelDegreeTable(std::vector<storage::RelTable*> relTables, common::RelDataDirection direction,
        planner::RelDegreeTableMode mode, DataPos nodeKeyOutputPos, DataPos degreeOutputPos,
        common::idx_t limit, physical_op_id id, std::unique_ptr<OPPrintInfo> printInfo)
        : PhysicalOperator{type_, id, std::move(printInfo)}, relTables{std::move(relTables)},
          direction{direction}, mode{mode}, nodeKeyOutputPos{nodeKeyOutputPos},
          degreeOutputPos{degreeOutputPos}, limit{limit} {}

    bool isSource() const override { return true; }
    bool isParallel() const override { return false; }

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;
    bool getNextTuplesInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<RelDegreeTable>(relTables, direction, mode, nodeKeyOutputPos,
            degreeOutputPos, limit, id, printInfo->copy());
    }

private:
    void writeNodeKey(common::offset_t offset, common::sel_t pos) const;

private:
    std::vector<storage::RelTable*> relTables;
    common::RelDataDirection direction;
    planner::RelDegreeTableMode mode;
    DataPos nodeKeyOutputPos;
    DataPos degreeOutputPos;
    common::idx_t limit;
    common::ValueVector* nodeKeyVector = nullptr;
    common::ValueVector* degreeVector = nullptr;
    bool hasExecuted = false;
};

} // namespace processor
} // namespace lbug

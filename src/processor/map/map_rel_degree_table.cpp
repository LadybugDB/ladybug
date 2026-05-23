#include "planner/operator/scan/logical_rel_degree_table.h"
#include "processor/operator/scan/rel_degree_table.h"
#include "processor/plan_mapper.h"
#include "storage/storage_manager.h"

using namespace lbug::common;
using namespace lbug::planner;
using namespace lbug::storage;

namespace lbug {
namespace processor {

std::unique_ptr<PhysicalOperator> PlanMapper::mapRelDegreeTable(
    const LogicalOperator* logicalOperator) {
    auto& logical = logicalOperator->constCast<LogicalRelDegreeTable>();
    auto outSchema = logical.getSchema();
    auto storageManager = StorageManager::Get(*clientContext);

    std::vector<RelTable*> relTables;
    for (auto tableID : logical.getRelTableIDs()) {
        relTables.push_back(storageManager->getTable(tableID)->ptrCast<RelTable>());
    }

    RelDataDirection relDirection;
    if (logical.getDirection() == ExtendDirection::FWD) {
        relDirection = RelDataDirection::FWD;
    } else if (logical.getDirection() == ExtendDirection::BWD) {
        relDirection = RelDataDirection::BWD;
    } else {
        relDirection = RelDataDirection::FWD;
    }

    DataPos nodeKeyOutputPos{};
    if (logical.getMode() == RelDegreeTableMode::TOP_K_DEGREES) {
        nodeKeyOutputPos = getDataPos(*logical.getNodeKeyExpr(), *outSchema);
    }
    auto degreeOutputPos = getDataPos(*logical.getDegreeExpr(), *outSchema);
    auto printInfo = std::make_unique<RelDegreeTablePrintInfo>(
        logical.getRelGroupEntry()->getName(), logical.getMode());
    return std::make_unique<RelDegreeTable>(std::move(relTables), relDirection, logical.getMode(),
        nodeKeyOutputPos, degreeOutputPos, logical.getLimit(), getOperatorID(),
        std::move(printInfo));
}

} // namespace processor
} // namespace lbug

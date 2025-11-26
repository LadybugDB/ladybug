#include "planner/operator/scan/logical_count_rel_table.h"
#include "processor/operator/scan/count_rel_table.h"
#include "processor/plan_mapper.h"
#include "storage/storage_manager.h"

using namespace lbug::common;
using namespace lbug::planner;
using namespace lbug::storage;

namespace lbug {
namespace processor {

std::unique_ptr<PhysicalOperator> PlanMapper::mapCountRelTable(
    const LogicalOperator* logicalOperator) {
    auto& countRelTable = logicalOperator->constCast<LogicalCountRelTable>();
    auto outSchema = countRelTable.getSchema();

    // Get the rel tables
    auto storageManager = StorageManager::Get(*clientContext);
    std::vector<RelTable*> relTables;
    for (auto tableID : countRelTable.getRelTableIDs()) {
        relTables.push_back(storageManager->getTable(tableID)->ptrCast<RelTable>());
    }

    // Get the output position for the count expression
    auto countOutputPos = getDataPos(*countRelTable.getCountExpr(), *outSchema);

    auto printInfo =
        std::make_unique<CountRelTablePrintInfo>(countRelTable.getRelGroupEntry()->getName());

    return std::make_unique<CountRelTable>(std::move(relTables), countOutputPos, getOperatorID(),
        std::move(printInfo));
}

} // namespace processor
} // namespace lbug

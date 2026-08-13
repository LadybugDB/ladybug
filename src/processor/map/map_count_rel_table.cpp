#include "common/constants.h"
#include "main/attached_database.h"
#include "main/database_manager.h"
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
    auto& logicalCountRelTable = logicalOperator->constCast<LogicalCountRelTable>();
    auto outSchema = logicalCountRelTable.getSchema();

    // Resolve the storage manager that owns the rel tables: main by default, or the
    // attached LBUG database recorded on the count operator for attached rels. Foreign
    // attached databases (sqlite/duckdb/postgres) are never routed here: the binder
    // resolves their rel names in the main catalog, so dbName stays empty for them.
    auto dbName = logicalCountRelTable.getDbName();
    auto storageManager = StorageManager::Get(*clientContext);
    if (!dbName.empty()) {
        auto* attachedDB = main::DatabaseManager::Get(*clientContext)->getAttachedDatabase(dbName);
        if (attachedDB->getDBType() == common::ATTACHED_LBUG_DB_TYPE) {
            auto* attachedLbug = static_cast<main::AttachedLbugDatabase*>(attachedDB);
            storageManager = attachedLbug->getStorageManager();
        }
    }

    // Get the node tables for scanning bound nodes
    std::vector<NodeTable*> nodeTables;
    for (auto tableID : logicalCountRelTable.getBoundNodeTableIDs()) {
        nodeTables.push_back(storageManager->getTable(tableID)->ptrCast<NodeTable>());
    }

    // Get the rel tables
    std::vector<RelTable*> relTables;
    for (auto tableID : logicalCountRelTable.getRelTableIDs()) {
        relTables.push_back(storageManager->getTable(tableID)->ptrCast<RelTable>());
    }

    // Determine rel data direction from extend direction
    auto extendDirection = logicalCountRelTable.getDirection();
    RelDataDirection relDirection;
    if (extendDirection == ExtendDirection::FWD) {
        relDirection = RelDataDirection::FWD;
    } else if (extendDirection == ExtendDirection::BWD) {
        relDirection = RelDataDirection::BWD;
    } else {
        // For BOTH, we'll scan FWD (shouldn't reach here as optimizer filters BOTH)
        relDirection = RelDataDirection::FWD;
    }

    // Get the output position for the count expression
    auto countOutputPos = getDataPos(*logicalCountRelTable.getCountExpr(), *outSchema);

    auto printInfo = std::make_unique<CountRelTablePrintInfo>(
        logicalCountRelTable.getRelGroupEntry()->getName());

    return std::make_unique<CountRelTable>(std::move(nodeTables), std::move(relTables),
        relDirection, countOutputPos, getOperatorID(), std::move(printInfo));
}

} // namespace processor
} // namespace lbug

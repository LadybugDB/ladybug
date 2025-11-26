#include "planner/operator/scan/logical_count_rel_table.h"
#include "processor/operator/scan/count_rel_table.h"
#include "processor/operator/scan/scan_node_table.h"
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

    auto storageManager = StorageManager::Get(*clientContext);
    auto transaction = transaction::Transaction::Get(*clientContext);

    // Get the rel tables
    std::vector<RelTable*> relTables;
    for (auto tableID : logicalCountRelTable.getRelTableIDs()) {
        relTables.push_back(storageManager->getTable(tableID)->ptrCast<RelTable>());
    }

    // Create SCAN_NODE_TABLE child operator for scanning bound nodes
    auto boundNode = logicalCountRelTable.getBoundNode();
    auto nodeIDPos = DataPos(outSchema->getExpressionPos(*boundNode->getInternalID()));

    std::vector<DataPos> emptyOutVectorsPos; // No properties needed
    auto scanInfo = ScanOpInfo(nodeIDPos, emptyOutVectorsPos);

    std::vector<ScanNodeTableInfo> tableInfos;
    std::vector<std::shared_ptr<ScanNodeTableSharedState>> sharedStates;
    std::vector<std::string> tableNames;

    for (auto tableID : logicalCountRelTable.getBoundNodeTableIDs()) {
        auto table = storageManager->getTable(tableID)->ptrCast<NodeTable>();
        tableNames.push_back(table->getTableName());

        // No properties, no predicates
        auto tableInfo = ScanNodeTableInfo(table, std::vector<ColumnPredicateSet>{});
        tableInfos.push_back(std::move(tableInfo));

        auto semiMask = SemiMaskUtil::createMask(table->getNumTotalRows(transaction));
        sharedStates.push_back(std::make_shared<ScanNodeTableSharedState>(std::move(semiMask)));
    }

    auto progressSharedState = std::make_shared<ScanNodeTableProgressSharedState>();
    auto scanNodePrintInfo = std::make_unique<ScanNodeTablePrintInfo>(tableNames,
        boundNode->getVariableName(), binder::expression_vector{});
    auto scanNodeTable = std::make_unique<ScanNodeTable>(std::move(scanInfo), std::move(tableInfos),
        std::move(sharedStates), getOperatorID(), std::move(scanNodePrintInfo),
        progressSharedState);

    // Determine rel data direction from extend direction
    auto extendDirection = logicalCountRelTable.getDirection();
    RelDataDirection relDirection;
    if (extendDirection == ExtendDirection::FWD) {
        relDirection = RelDataDirection::FWD;
    } else if (extendDirection == ExtendDirection::BWD) {
        relDirection = RelDataDirection::BWD;
    } else {
        // For BOTH, we'll scan FWD (the implementation would need to handle both directions)
        relDirection = RelDataDirection::FWD;
    }

    // Get the output position for the count expression
    auto countOutputPos = getDataPos(*logicalCountRelTable.getCountExpr(), *outSchema);

    auto printInfo = std::make_unique<CountRelTablePrintInfo>(
        logicalCountRelTable.getRelGroupEntry()->getName());

    return std::make_unique<CountRelTable>(std::move(relTables), relDirection, nodeIDPos,
        countOutputPos, std::move(scanNodeTable), getOperatorID(), std::move(printInfo));
}

} // namespace processor
} // namespace lbug

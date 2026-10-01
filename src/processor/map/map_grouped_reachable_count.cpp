#include "binder/expression/property_expression.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "main/client_context.h"
#include "planner/operator/scan/logical_grouped_reachable_count.h"
#include "processor/operator/scan/grouped_reachable_count.h"
#include "processor/plan_mapper.h"
#include "storage/storage_manager.h"

using namespace lbug::binder;
using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::graph;
using namespace lbug::planner;

namespace lbug {
namespace processor {

std::unique_ptr<PhysicalOperator> PlanMapper::mapGroupedReachableCount(
    const LogicalOperator* logicalOperator) {
    auto& logical = logicalOperator->constCast<LogicalGroupedReachableCount>();
    auto outSchema = logical.getSchema();

    auto boundNode = logical.getBoundNode();
    auto nbrNode = logical.getNbrNode();
    DASSERT(boundNode->getNumEntries() == 1 && nbrNode->getNumEntries() == 1);
    auto boundTableID = boundNode->getTableIDs()[0];
    auto nbrTableID = nbrNode->getTableIDs()[0];

    std::vector<TableCatalogEntry*> nodeEntries{boundNode->getEntry(0), nbrNode->getEntry(0)};
    std::vector<TableCatalogEntry*> relEntries{logical.getRelGroupEntry()};
    auto graphEntry = NativeGraphEntry(std::move(nodeEntries), std::move(relEntries));

    DASSERT(logical.getDirection() == ExtendDirection::FWD);
    RelDataDirection direction = RelDataDirection::FWD;

    auto* storageManager = storage::StorageManager::Get(*clientContext);
    auto* nodeTable = storageManager->getTable(nbrTableID)->ptrCast<storage::NodeTable>();
    auto* nodeEntry = nbrNode->getEntry(0)->ptrCast<catalog::NodeTableCatalogEntry>();

    std::vector<GroupedReachableCount::KeyInfo> keyInfos;
    keyInfos.reserve(logical.getKeys().size());
    std::vector<DataPos> keyOutputPoss;
    keyOutputPoss.reserve(logical.getKeys().size());
    for (auto& key : logical.getKeys()) {
        auto& prop = key->constCast<PropertyExpression>();
        GroupedReachableCount::KeyInfo info;
        info.logicalType = key->getDataType().copy();
        if (prop.isInternalID()) {
            info.isInternalID = true;
        } else {
            info.columnID = nodeEntry->getColumnID(prop.getPropertyName());
        }
        keyInfos.push_back(std::move(info));
        keyOutputPoss.push_back(getDataPos(*key, *outSchema));
    }

    column_id_t scoreColumnID = INVALID_COLUMN_ID;
    LogicalType scoreType;
    if (logical.hasAvg()) {
        auto& prop = logical.getAvgChild()->constCast<PropertyExpression>();
        scoreColumnID = nodeEntry->getColumnID(prop.getPropertyName());
        scoreType = logical.getAvgChild()->getDataType().copy();
    }

    DataPos countOutputPos;
    bool hasCount = logical.hasCount();
    if (hasCount) {
        countOutputPos = getDataPos(*logical.getCountExpr(), *outSchema);
    }
    DataPos avgOutputPos;
    bool hasAvg = logical.hasAvg();
    if (hasAvg) {
        avgOutputPos = getDataPos(*logical.getAvgExpr(), *outSchema);
    }
    auto printInfo = std::make_unique<GroupedReachableCountPrintInfo>(
        logical.getRelGroupEntry()->getName(), logical.getLowerBound(), logical.getUpperBound(),
        logical.getKeys().size(), hasCount, hasAvg);
    return std::make_unique<GroupedReachableCount>(std::move(graphEntry), direction,
        logical.getLowerBound(), logical.getUpperBound(), boundTableID, nbrTableID, nodeTable,
        std::move(keyInfos), scoreColumnID, std::move(scoreType), std::move(keyOutputPoss),
        std::move(countOutputPos), hasCount, std::move(avgOutputPos), hasAvg, getOperatorID(),
        std::move(printInfo));
}

} // namespace processor
} // namespace lbug

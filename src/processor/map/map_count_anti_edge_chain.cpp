#include "main/client_context.h"
#include "planner/operator/scan/logical_count_anti_edge_chain.h"
#include "processor/operator/scan/count_anti_edge_chain.h"
#include "processor/plan_mapper.h"
#include "storage/storage_manager.h"

using namespace lbug::common;
using namespace lbug::planner;
using namespace lbug::storage;

namespace lbug {
namespace processor {

std::unique_ptr<PhysicalOperator> PlanMapper::mapCountAntiEdgeChain(
    const LogicalOperator* logicalOperator) {
    auto& logicalAntiEdgeChain = logicalOperator->constCast<LogicalCountAntiEdgeChain>();
    auto outSchema = logicalAntiEdgeChain.getSchema();
    auto countOutputPos = getDataPos(*logicalAntiEdgeChain.getCountExpr(), *outSchema);

    auto* storageManager = StorageManager::Get(*clientContext);
    std::vector<CountAntiEdgeChain::Hop> hops;
    hops.reserve(logicalAntiEdgeChain.getSuffixHops().size());
    for (auto& logicalHop : logicalAntiEdgeChain.getSuffixHops()) {
        CountAntiEdgeChain::Hop hop;
        hop.relTables.reserve(logicalHop.relScans.size());
        hop.scanDirections.reserve(logicalHop.relScans.size());
        hop.fromNodeTables.reserve(logicalHop.relScans.size());
        hop.toNodeTables.reserve(logicalHop.relScans.size());
        for (auto& spec : logicalHop.relScans) {
            hop.relTables.push_back(storageManager->getTable(spec.relTableID)->ptrCast<RelTable>());
            hop.scanDirections.push_back(spec.scanDirection);
            hop.fromNodeTables.push_back(
                storageManager->getTable(spec.fromNodeTableID)->ptrCast<NodeTable>());
            hop.toNodeTables.push_back(
                storageManager->getTable(spec.toNodeTableID)->ptrCast<NodeTable>());
        }
        hops.push_back(std::move(hop));
    }

    auto* antiRelTable =
        storageManager->getTable(logicalAntiEdgeChain.getAntiRelTableIDs()[0])->ptrCast<RelTable>();
    auto* midNodeTable =
        storageManager->getTable(logicalAntiEdgeChain.getMidNodeTableID())->ptrCast<NodeTable>();

    return std::make_unique<CountAntiEdgeChain>(std::move(hops), antiRelTable, midNodeTable,
        logicalAntiEdgeChain.getChainN0Dir(), logicalAntiEdgeChain.getChainN2Dir(),
        logicalAntiEdgeChain.getAntiEdgeDir(), logicalAntiEdgeChain.getHasNotEquals(),
        countOutputPos, getOperatorID(), logicalAntiEdgeChain.getPrintInfo());
}

} // namespace processor
} // namespace lbug

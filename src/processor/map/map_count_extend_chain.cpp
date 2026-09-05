#include "main/client_context.h"
#include "planner/operator/scan/logical_count_extend_chain.h"
#include "processor/operator/scan/count_extend_chain.h"
#include "processor/plan_mapper.h"
#include "storage/storage_manager.h"

using namespace lbug::common;
using namespace lbug::planner;
using namespace lbug::storage;

namespace lbug {
namespace processor {

std::unique_ptr<PhysicalOperator> PlanMapper::mapCountExtendChain(
    const LogicalOperator* logicalOperator) {
    auto& logicalCountExtendChain = logicalOperator->constCast<LogicalCountExtendChain>();
    auto outSchema = logicalCountExtendChain.getSchema();
    auto countOutputPos = getDataPos(*logicalCountExtendChain.getCountExpr(), *outSchema);

    auto* storageManager = StorageManager::Get(*clientContext);
    std::unordered_map<table_id_t, NodeTable*> nodeTables;
    std::vector<CountExtendChain::Hop> hops;
    hops.reserve(logicalCountExtendChain.getHops().size());
    for (auto& logicalHop : logicalCountExtendChain.getHops()) {
        CountExtendChain::Hop hop;
        hop.relTables.reserve(logicalHop.relScans.size());
        hop.scanDirections.reserve(logicalHop.relScans.size());
        hop.fromNodeTableIDs.reserve(logicalHop.relScans.size());
        hop.toNodeTableIDs.reserve(logicalHop.relScans.size());
        for (auto& spec : logicalHop.relScans) {
            hop.relTables.push_back(storageManager->getTable(spec.relTableID)->ptrCast<RelTable>());
            hop.scanDirections.push_back(spec.scanDirection);
            hop.fromNodeTableIDs.push_back(spec.fromNodeTableID);
            hop.toNodeTableIDs.push_back(spec.toNodeTableID);
            if (!nodeTables.contains(spec.fromNodeTableID)) {
                nodeTables.emplace(spec.fromNodeTableID,
                    storageManager->getTable(spec.fromNodeTableID)->ptrCast<NodeTable>());
            }
            if (!nodeTables.contains(spec.toNodeTableID)) {
                nodeTables.emplace(spec.toNodeTableID,
                    storageManager->getTable(spec.toNodeTableID)->ptrCast<NodeTable>());
            }
        }
        hops.push_back(std::move(hop));
    }

    return std::make_unique<CountExtendChain>(std::move(hops), std::move(nodeTables),
        countOutputPos, getOperatorID(), logicalCountExtendChain.getPrintInfo());
}

} // namespace processor
} // namespace lbug

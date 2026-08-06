#include "planner/operator/scan/logical_reachable_count.h"
#include "processor/operator/scan/reachable_count.h"
#include "processor/plan_mapper.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::graph;
using namespace lbug::planner;

namespace lbug {
namespace processor {

std::unique_ptr<PhysicalOperator> PlanMapper::mapReachableCount(
    const LogicalOperator* logicalOperator) {
    auto& logical = logicalOperator->constCast<LogicalReachableCount>();
    auto outSchema = logical.getSchema();

    auto boundNode = logical.getBoundNode();
    auto nbrNode = logical.getNbrNode();
    DASSERT(boundNode->getNumEntries() == 1 && nbrNode->getNumEntries() == 1);
    auto boundTableID = boundNode->getTableIDs()[0];
    auto nbrTableID = nbrNode->getTableIDs()[0];

    std::vector<TableCatalogEntry*> nodeEntries{boundNode->getEntry(0), nbrNode->getEntry(0)};
    std::vector<TableCatalogEntry*> relEntries{logical.getRelGroupEntry()};
    auto graphEntry = NativeGraphEntry(std::move(nodeEntries), std::move(relEntries));

    RelDataDirection direction = logical.getDirection() == ExtendDirection::BWD ?
                                     RelDataDirection::BWD :
                                     RelDataDirection::FWD;

    auto countOutputPos = getDataPos(*logical.getCountExpr(), *outSchema);
    auto printInfo = std::make_unique<ReachableCountPrintInfo>(
        logical.getRelGroupEntry()->getName(), logical.getLowerBound(), logical.getUpperBound());
    return std::make_unique<ReachableCount>(std::move(graphEntry), direction,
        logical.getLowerBound(), logical.getUpperBound(), boundTableID, nbrTableID,
        logical.getStartOffsets(), countOutputPos, getOperatorID(), std::move(printInfo));
}

} // namespace processor
} // namespace lbug

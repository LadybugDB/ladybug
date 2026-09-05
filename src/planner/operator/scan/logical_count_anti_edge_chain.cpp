#include "planner/operator/scan/logical_count_anti_edge_chain.h"

namespace lbug {
namespace planner {

void LogicalCountAntiEdgeChain::computeFactorizedSchema() {
    createEmptySchema();
    auto groupPos = schema->createGroup();
    schema->insertToGroupAndScope(countExpr, groupPos);
    schema->setGroupAsSingleState(groupPos);
}

void LogicalCountAntiEdgeChain::computeFlatSchema() {
    createEmptySchema();
    auto groupPos = schema->createGroup();
    schema->insertToGroupAndScope(countExpr, groupPos);
}

} // namespace planner
} // namespace lbug

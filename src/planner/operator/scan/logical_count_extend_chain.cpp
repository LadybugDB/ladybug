#include "planner/operator/scan/logical_count_extend_chain.h"

namespace lbug {
namespace planner {

void LogicalCountExtendChain::computeFactorizedSchema() {
    createEmptySchema();
    // This operator is a source - it has no child in the logical plan.
    auto groupPos = schema->createGroup();
    schema->insertToGroupAndScope(countExpr, groupPos);
    schema->setGroupAsSingleState(groupPos);
}

void LogicalCountExtendChain::computeFlatSchema() {
    createEmptySchema();
    auto groupPos = schema->createGroup();
    schema->insertToGroupAndScope(countExpr, groupPos);
}

} // namespace planner
} // namespace lbug

#include "planner/operator/scan/logical_reachable_count.h"

namespace lbug {
namespace planner {

void LogicalReachableCount::computeFactorizedSchema() {
    createEmptySchema();
    auto groupPos = schema->createGroup();
    schema->insertToGroupAndScope(countExpr, groupPos);
    schema->setGroupAsSingleState(groupPos);
}

void LogicalReachableCount::computeFlatSchema() {
    createEmptySchema();
    auto groupPos = schema->createGroup();
    schema->insertToGroupAndScope(countExpr, groupPos);
}

} // namespace planner
} // namespace lbug

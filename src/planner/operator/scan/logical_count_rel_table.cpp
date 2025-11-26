#include "planner/operator/scan/logical_count_rel_table.h"

namespace lbug {
namespace planner {

void LogicalCountRelTable::computeFactorizedSchema() {
    createEmptySchema();
    auto groupPos = schema->createGroup();
    // Add bound node's internal ID for the child scan
    schema->insertToGroupAndScope(boundNode->getInternalID(), groupPos);
    schema->insertToGroupAndScope(countExpr, groupPos);
    schema->setGroupAsSingleState(groupPos);
}

void LogicalCountRelTable::computeFlatSchema() {
    createEmptySchema();
    schema->createGroup();
    // Add bound node's internal ID for the child scan
    schema->insertToGroupAndScope(boundNode->getInternalID(), 0 /* groupPos */);
    schema->insertToGroupAndScope(countExpr, 0 /* groupPos */);
}

} // namespace planner
} // namespace lbug

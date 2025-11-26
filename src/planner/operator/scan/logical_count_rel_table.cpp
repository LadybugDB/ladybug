#include "planner/operator/scan/logical_count_rel_table.h"

namespace lbug {
namespace planner {

void LogicalCountRelTable::computeFactorizedSchema() {
    createEmptySchema();
    // Only output the count expression in a single-state group.
    // This operator is a source - it has no child in the logical plan.
    // The bound node is used internally for scanning but not exposed.
    auto groupPos = schema->createGroup();
    schema->insertToGroupAndScope(countExpr, groupPos);
    schema->setGroupAsSingleState(groupPos);
}

void LogicalCountRelTable::computeFlatSchema() {
    createEmptySchema();
    // For flat schema, create a single group with the count expression.
    auto groupPos = schema->createGroup();
    schema->insertToGroupAndScope(countExpr, groupPos);
}

} // namespace planner
} // namespace lbug

#include "planner/operator/scan/logical_count_rel_table.h"

namespace lbug {
namespace planner {

void LogicalCountRelTable::computeFactorizedSchema() {
    createEmptySchema();
    auto groupPos = schema->createGroup();
    schema->insertToGroupAndScope(countExpr, groupPos);
    schema->setGroupAsSingleState(groupPos);
}

void LogicalCountRelTable::computeFlatSchema() {
    createEmptySchema();
    schema->createGroup();
    schema->insertToGroupAndScope(countExpr, 0 /* groupPos */);
}

} // namespace planner
} // namespace lbug

#include "planner/operator/scan/logical_rel_degree_table.h"

namespace lbug {
namespace planner {

void LogicalRelDegreeTable::computeFactorizedSchema() {
    createEmptySchema();
    auto groupPos = schema->createGroup();
    if (mode == RelDegreeTableMode::TOP_K_DEGREES) {
        schema->insertToGroupAndScope(nodeKeyExpr, groupPos);
    }
    schema->insertToGroupAndScope(degreeExpr, groupPos);
    if (mode == RelDegreeTableMode::ACTIVE_BOUND_COUNT) {
        schema->setGroupAsSingleState(groupPos);
    }
}

void LogicalRelDegreeTable::computeFlatSchema() {
    createEmptySchema();
    auto groupPos = schema->createGroup();
    if (mode == RelDegreeTableMode::TOP_K_DEGREES) {
        schema->insertToGroupAndScope(nodeKeyExpr, groupPos);
    }
    schema->insertToGroupAndScope(degreeExpr, groupPos);
}

} // namespace planner
} // namespace lbug

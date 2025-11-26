#include "planner/operator/scan/logical_count_rel_table.h"

namespace lbug {
namespace planner {

void LogicalCountRelTable::computeFactorizedSchema() {
    createEmptySchema();
    // The bound node needs a full-capacity group for scanning (not single state)
    auto boundNodeGroupPos = schema->createGroup();
    schema->insertToGroupAndScope(boundNode->getInternalID(), boundNodeGroupPos);
    // The count expression goes in its own single-state group
    auto countGroupPos = schema->createGroup();
    schema->insertToGroupAndScope(countExpr, countGroupPos);
    schema->setGroupAsSingleState(countGroupPos);
}

void LogicalCountRelTable::computeFlatSchema() {
    createEmptySchema();
    // For flat schema, put everything in group 0 (required by LogicalProjection::computeFlatSchema
    // which hardcodes groupPos=0 when calling insertToScopeMayRepeat after copyChildSchema)
    auto groupPos = schema->createGroup();
    schema->insertToGroupAndScope(boundNode->getInternalID(), groupPos);
    schema->insertToGroupAndScope(countExpr, groupPos);
}

} // namespace planner
} // namespace lbug

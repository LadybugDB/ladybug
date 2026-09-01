#include "planner/operator/scan/logical_query_primary_key_lookup.h"

#include "planner/operator/factorization/flatten_resolver.h"

namespace lbug {
namespace planner {

// The group the lookup writes its node ID and properties into must be the group the key is read
// from: the physical operator drives the child through that group's DataChunkState and zips the
// key's selection against it, so a mismatch means it reads an empty selection and emits nothing.
//
// `outputGroupPos` records the key's group as it was when this operator was created. That index
// does not survive the plan below being rebuilt -- joining a correlated MATCH underneath it
// renumbers the groups -- so it is only a fallback here. Resolving the group from the schema we
// actually have keeps the two in step however the plan was assembled.
f_group_pos LogicalQueryPrimaryKeyLookup::resolveOutputGroupPos() const {
    auto analyzer = GroupDependencyAnalyzer(false /* collectDependentExpr */, *schema);
    analyzer.visit(key);
    const auto dependentGroups = analyzer.getDependentGroups();
    if (dependentGroups.size() == 1) {
        return *dependentGroups.begin();
    }
    return outputGroupPos;
}

void LogicalQueryPrimaryKeyLookup::computeFactorizedSchema() {
    copyChildSchema(0);
    const auto groupPos = resolveOutputGroupPos();
    schema->insertToGroupAndScope(nodeID, groupPos);
    for (auto& property : properties) {
        schema->insertToGroupAndScope(property, groupPos);
    }
}

void LogicalQueryPrimaryKeyLookup::computeFlatSchema() {
    copyChildSchema(0);
    schema->insertToGroupAndScope(nodeID, 0);
    for (auto& property : properties) {
        schema->insertToGroupAndScope(property, 0);
    }
}

} // namespace planner
} // namespace lbug

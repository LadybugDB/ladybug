#include "planner/operator/scan/logical_query_primary_key_lookup.h"

namespace lbug {
namespace planner {

void LogicalQueryPrimaryKeyLookup::computeFactorizedSchema() {
    copyChildSchema(0);
    schema->insertToGroupAndScope(nodeID, outputGroupPos);
    for (auto& property : properties) {
        schema->insertToGroupAndScope(property, outputGroupPos);
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

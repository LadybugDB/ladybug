#include "binder/expression/node_expression.h"
#include "binder/expression/property_expression.h"
#include "binder/visitor/property_collector.h"
#include "planner/operator/logical_hash_join.h"
#include "planner/operator/scan/logical_query_primary_key_lookup.h"
#include "planner/planner.h"

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace planner {

// Note: we cannot append ResultCollector for plans enumerated for single query before there could
// be a UNION on top which requires further flatten. So we delay ResultCollector appending to
// enumerate regular query level.
LogicalPlan Planner::planSingleQuery(const NormalizedSingleQuery& singleQuery) {
    auto propertyCollector = PropertyCollector();
    propertyCollector.visitSingleQuery(singleQuery);
    // Late properties are read by planLatePropertyFetch, not by the node's table scan.
    auto lateProperties = expression_set{};
    for (auto i = 0u; i < singleQuery.getNumQueryParts(); ++i) {
        auto partProperties = singleQuery.getQueryPart(i)->getLateProperties();
        lateProperties.insert(partProperties.begin(), partProperties.end());
    }
    for (auto& expr : propertyCollector.getProperties()) {
        if (lateProperties.contains(expr)) {
            continue;
        }
        auto& property = expr->constCast<PropertyExpression>();
        propertyExprCollection.addProperties(property.getVariableName(), expr);
    }
    context.resetState();
    auto plan = LogicalPlan();
    for (auto i = 0u; i < singleQuery.getNumQueryParts(); ++i) {
        planQueryPart(*singleQuery.getQueryPart(i), plan);
    }
    return plan;
}

void Planner::planQueryPart(const NormalizedQueryPart& queryPart, LogicalPlan& plan) {
    for (auto& fetch : queryPart.getLatePropertyFetches()) {
        planLatePropertyFetch(fetch, plan);
    }
    // plan read
    for (auto i = 0u; i < queryPart.getNumReadingClause(); i++) {
        planReadingClause(*queryPart.getReadingClause(i), plan);
    }
    // plan update
    for (auto i = 0u; i < queryPart.getNumUpdatingClause(); ++i) {
        planUpdatingClause(*queryPart.getUpdatingClause(i), plan);
    }
    // plan projection
    if (queryPart.hasProjectionBody()) {
        planProjectionBody(queryPart.getProjectionBody(), plan);
        if (queryPart.hasProjectionBodyPredicate()) {
            appendFilter(queryPart.getProjectionBodyPredicate(), plan);
        }
    }
}

void Planner::planLatePropertyFetch(const LatePropertyFetch& fetch, LogicalPlan& plan) {
    auto& node = *fetch.node;
    auto nodeID = node.getInternalID();
    // Label pruning after the rewrite can leave the node without a table. The query then has no
    // rows, and a join with an empty scan plans that.
    if (fetch.useLookup && node.getNumEntries() == 1) {
        appendFlattens(plan.getSchema()->getGroupsPosInScope(), plan);
        auto lookup = std::make_shared<LogicalQueryPrimaryKeyLookup>(node.getTableIDs()[0], nodeID,
            fetch.properties, nodeID, plan.getSchema()->getGroupPos(*nodeID),
            plan.getLastOperator());
        lookup->computeFactorizedSchema();
        lookup->setCardinality(plan.getCardinality());
        plan.setLastOperator(std::move(lookup));
        return;
    }
    // Build on the fetched rows, whose node IDs mask the probe-side scan.
    auto scanPlan = LogicalPlan();
    appendScanNodeTable(nodeID, node.getTableIDs(), fetch.properties, scanPlan, &node);
    appendHashJoin(expression_vector{nodeID}, JoinType::INNER, scanPlan, plan, plan);
    auto& sipInfo = plan.getLastOperator()->cast<LogicalHashJoin>().getSIPInfoUnsafe();
    sipInfo.position = SemiMaskPosition::PROHIBIT_PROBE_TO_BUILD;
    sipInfo.direction = SIPDirection::FORCE_BUILD_TO_PROBE;
}

} // namespace planner
} // namespace lbug

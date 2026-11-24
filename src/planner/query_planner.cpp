#include "binder/query/bound_regular_query.h"
#include "planner/operator/logical_union.h"
#include "planner/planner.h"

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace planner {

LogicalPlan Planner::planQuery(const BoundStatement& boundStatement) {
    auto& regularQuery = boundStatement.constCast<BoundRegularQuery>();
    if (regularQuery.getNumSingleQueries() == 1) {
        return planSingleQuery(*regularQuery.getSingleQuery(0));
    }
    std::vector<LogicalPlan> childrenPlans;
    std::vector<binder::expression_vector> childProjections;
    for (auto i = 0u; i < regularQuery.getNumSingleQueries(); i++) {
        childrenPlans.push_back(planSingleQuery(*regularQuery.getSingleQuery(i)));
        // Each arm carries its exact non-deduplicated column list, validated by
        // validateUnionColumnsOfTheSameType to match expressionsToUnion in arity. Using it
        // directly (instead of walking the operator tree for the last LogicalProjection) is
        // immune to new operators above the projection or optimizer rewrites, and guarantees
        // childProjections[i].size() == expressionsToUnion.size() by construction (see #1693).
        childProjections.push_back(
            regularQuery.getSingleQuery(i)->getStatementResult()->getColumns());
    }
    auto exprs = regularQuery.getStatementResult()->getColumns();
    return createUnionPlan(childrenPlans, exprs, childProjections, regularQuery.getIsUnionAll(0));
}

LogicalPlan Planner::createUnionPlan(std::vector<LogicalPlan>& childrenPlans,
    const expression_vector& expressions,
    const std::vector<binder::expression_vector>& childProjections, bool isUnionAll) {
    DASSERT(!childrenPlans.empty());
    DASSERT(childrenPlans.size() == childProjections.size());
    auto plan = LogicalPlan();
    std::vector<std::shared_ptr<LogicalOperator>> children;
    children.reserve(childrenPlans.size());
    for (auto& childPlan : childrenPlans) {
        children.push_back(childPlan.getLastOperator());
    }
    // we compute the schema based on first child
    auto union_ = std::make_shared<LogicalUnion>(expressions, std::move(children));
    union_->setChildProjections(childProjections);
    for (auto i = 0u; i < childrenPlans.size(); ++i) {
        appendFlattens(union_->getGroupsPosToFlatten(i), childrenPlans[i]);
        union_->setChild(i, childrenPlans[i].getLastOperator());
    }
    union_->computeFactorizedSchema();
    plan.setLastOperator(union_);
    if (!isUnionAll && !expressions.empty()) {
        appendDistinct(expressions, plan);
    }
    return plan;
}

expression_vector Planner::getProperties(const Expression& pattern) const {
    DASSERT(pattern.expressionType == ExpressionType::PATTERN);
    return propertyExprCollection.getProperties(pattern);
}

JoinOrderEnumeratorContext Planner::enterNewContext() {
    auto prevContext = std::move(context);
    context = JoinOrderEnumeratorContext();
    return prevContext;
}

void Planner::exitContext(JoinOrderEnumeratorContext prevContext) {
    context = std::move(prevContext);
}

PropertyExprCollection Planner::enterNewPropertyExprCollection() {
    auto prevCollection = std::move(propertyExprCollection);
    propertyExprCollection = PropertyExprCollection();
    return prevCollection;
}

void Planner::exitPropertyExprCollection(PropertyExprCollection collection) {
    propertyExprCollection = std::move(collection);
}

} // namespace planner
} // namespace lbug

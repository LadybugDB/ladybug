#include "planner/operator/logical_aggregate.h"
#include "planner/planner.h"

using namespace lbug::binder;

namespace lbug {
namespace planner {

void Planner::appendAggregate(const expression_vector& expressionsToGroupBy,
    const expression_vector& expressionsToAggregate, LogicalPlan& plan) {
    // Collapse independent LEFT legs before they compound: COUNT(DISTINCT) per leg
    // grouped by the same keys (Q14's four OPTIONAL diamonds -> 656k crossed rows).
    if (tryPreAggregateDistinctLeftChain(expressionsToGroupBy, expressionsToAggregate, plan)) {
        return;
    }
    auto aggregate = make_shared<LogicalAggregate>(expressionsToGroupBy, expressionsToAggregate,
        plan.getLastOperator());
    appendFlattens(aggregate->getGroupsPosToFlatten(), plan);
    aggregate->setChild(0, plan.getLastOperator());
    aggregate->computeFactorizedSchema();
    aggregate->setCardinality(cardinalityEstimator.estimateAggregate(*aggregate));
    plan.setLastOperator(std::move(aggregate));
}

} // namespace planner
} // namespace lbug

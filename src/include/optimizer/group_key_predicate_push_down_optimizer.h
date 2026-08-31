#pragma once

#include <memory>
#include <string>
#include <unordered_set>

#include "binder/expression/expression.h"
#include "planner/operator/logical_plan.h"

namespace lbug {
namespace optimizer {

// Pushes predicates from a WITH ... WHERE clause below an aggregate when every value referenced
// by the predicate is a grouping key of that aggregate. In particular, this turns plans shaped as
//
//   FILTER(a = b) -> PROJECTION -> AGGREGATE -> ... -> CROSS_PRODUCT
//
// into plans where the filter is directly above the CROSS_PRODUCT. The regular filter push-down
// pass can then rewrite the cross product plus equality predicate into a hash join.
class GroupKeyPredicatePushDownOptimizer {
public:
    void rewrite(planner::LogicalPlan* plan);

private:
    std::shared_ptr<planner::LogicalOperator> visitOperator(
        std::shared_ptr<planner::LogicalOperator> op);
    std::shared_ptr<planner::LogicalOperator> tryRewriteFilter(
        std::shared_ptr<planner::LogicalOperator> op);

    static std::shared_ptr<planner::LogicalOperator> findAggregateThroughProjections(
        const std::shared_ptr<planner::LogicalOperator>& op);
    static bool isGroupKeyOnlyPredicate(const binder::Expression& predicate,
        const std::unordered_set<std::string>& groupKeyNames);
    static bool isComposedFromGroupKeys(const binder::Expression& expression,
        const std::unordered_set<std::string>& groupKeyNames);
    static bool containsRandomFunction(const binder::Expression& expression);

    std::shared_ptr<planner::LogicalOperator> pushPredicate(
        std::shared_ptr<planner::LogicalOperator> op,
        const std::shared_ptr<binder::Expression>& predicate);
    static std::shared_ptr<planner::LogicalOperator> appendFilter(
        std::shared_ptr<planner::LogicalOperator> child,
        const std::shared_ptr<binder::Expression>& predicate,
        common::cardinality_t cardinality = 0);
    static void recomputeFlatSchemas(const std::shared_ptr<planner::LogicalOperator>& op);
};

} // namespace optimizer
} // namespace lbug

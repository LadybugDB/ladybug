#pragma once

#include "planner/operator/logical_operator.h"
#include "planner/operator/logical_plan.h"

namespace lbug {
namespace optimizer {

/**
 * Removes ORDER BY operators that don't affect the query result.
 *
 * Pattern detected (Issue #720):
 *   AGGREGATE (no GROUP BY keys, e.g. COUNT(*)) ->
 *     ... (PROJECTION, MULTIPLICITY_REDUCER, LIMIT, FILTER) ->
 *       ORDER BY
 *
 * When the aggregate has no GROUP BY keys, ordering the input rows doesn't
 * change the aggregate result (e.g. COUNT(*) returns the same number of rows
 * regardless of order). The ORDER BY is therefore unnecessary and can be removed.
 *
 * If the ORDER BY had an absorbed LIMIT (i.e., it was a TOP_K), the LIMIT is
 * preserved by re-creating a LIMIT operator so the row count stays correct.
 *
 * This optimizer runs after FactorizationRewriter (the plan is in factorized
 * form), so it recomputes the factorized schema after rewrites.
 */
class RemoveUnnecessaryOrderByOptimizer {
public:
    void rewrite(planner::LogicalPlan* plan);

private:
    std::shared_ptr<planner::LogicalOperator> visitOperator(
        std::shared_ptr<planner::LogicalOperator> op);
    // For a keyless AGGREGATE, strip an ORDER BY sitting directly below it
    // (possibly through passthrough operators). Returns the (possibly replaced)
    // aggregate.
    std::shared_ptr<planner::LogicalOperator> tryRemoveOrderByFromAggregate(
        std::shared_ptr<planner::LogicalOperator> op);
    // Walk down the child chain looking for an ORDER BY to remove. Returns the
    // (possibly replaced) subtree root and sets `changed` to true if a removal
    // was performed. Only traverses single-child passthrough operators
    // (PROJECTION, MULTIPLICITY_REDUCER, LIMIT, FILTER); stops at anything else.
    std::shared_ptr<planner::LogicalOperator> stripOrderByBelow(
        std::shared_ptr<planner::LogicalOperator> op, bool& changed);
};

} // namespace optimizer
} // namespace lbug

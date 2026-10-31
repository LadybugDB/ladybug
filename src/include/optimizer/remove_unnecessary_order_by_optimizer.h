#pragma once

#include "planner/operator/logical_operator.h"
#include "planner/operator/logical_plan.h"

namespace lbug {
namespace optimizer {

/**
 * Removes ORDER BY operators that don't affect the query result.
 *
 * Pattern detected (Issue #720):
 *   AGGREGATE (no GROUP BY keys, only COUNT(*)) ->
 *     ... (PROJECTION, MULTIPLICITY_REDUCER, LIMIT) ->
 *       ORDER BY
 *
 * With no FILTER in between, COUNT(*) returns the same number whichever rows the
 * ORDER BY's SKIP/LIMIT keeps, so the ORDER BY is unnecessary and can be removed.
 * Any other aggregate reads the kept rows, so it keeps the ORDER BY.
 *
 * If the ORDER BY had an absorbed LIMIT (i.e., it was a TOP_K), the LIMIT and its
 * SKIP are preserved by re-creating a LIMIT operator, over a MULTIPLICITY_REDUCER,
 * so the row count stays correct.
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
    // For a keyless COUNT(*) AGGREGATE, strip an ORDER BY below it
    // (possibly through passthrough operators). Returns the (possibly replaced)
    // aggregate.
    std::shared_ptr<planner::LogicalOperator> tryRemoveOrderByFromAggregate(
        std::shared_ptr<planner::LogicalOperator> op);
    // Walk down the child chain looking for an ORDER BY to remove. Returns the
    // (possibly replaced) subtree root and sets `changed` to true if a removal
    // was performed. Only traverses single-child passthrough operators
    // (PROJECTION, MULTIPLICITY_REDUCER, LIMIT); stops at anything else.
    std::shared_ptr<planner::LogicalOperator> stripOrderByBelow(
        std::shared_ptr<planner::LogicalOperator> op, bool& changed);
};

} // namespace optimizer
} // namespace lbug

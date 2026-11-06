#pragma once

#include "planner/operator/logical_distinct.h"
#include "planner/operator/logical_operator.h"
#include "planner/operator/logical_plan.h"

namespace lbug {
namespace optimizer {

/**
 * Removes DISTINCT operators that are redundant because the input is already
 * guaranteed to be unique on the distinct keys.
 *
 * Pattern detected (Issue #721):
 *   ... (any parent) -> DISTINCT(keys) -> ... -> SCAN_NODE_TABLE
 *
 * A DISTINCT is only removed when BOTH of the following hold:
 *   1. Every distinct key is a primary-key property or the internal ID of a
 *      single scanned node table (such a key uniquely identifies a row).
 *   2. The DISTINCT's input has multiplicity 1, i.e. its child subtree is a
 *      linear chain of non-multiplying operators (PROJECTION, FILTER,
 *      MULTIPLICITY_REDUCER, LIMIT, ...) ending at a plain
 *      SCAN_NODE_TABLE. Joins, extends, recursive/path scans, UNWIND and
 *      materialized-subplan reads (EXPRESSIONS_SCAN/TABLE_FUNCTION_CALL) can all
 *      duplicate rows, so a DISTINCT above them is kept.
 *
 * This optimizer runs before FactorizationRewriter (the plan is flat), so it
 * recomputes the flat schema after rewrites, and FactorizationRewriter places
 * flattens only for the operators that remain.
 */
class RemoveUnnecessaryDistinctOptimizer {
public:
    void rewrite(planner::LogicalPlan* plan);

private:
    std::shared_ptr<planner::LogicalOperator> visitOperator(
        std::shared_ptr<planner::LogicalOperator> op);
    std::shared_ptr<planner::LogicalOperator> visitOperatorReplaceSwitch(
        std::shared_ptr<planner::LogicalOperator> op);
    bool isRedundantDistinct(const planner::LogicalDistinct& distinct);
    bool isChildMultiplicityOne(std::shared_ptr<planner::LogicalOperator> op);
};

} // namespace optimizer
} // namespace lbug

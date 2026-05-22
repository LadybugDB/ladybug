#pragma once

#include "logical_operator_visitor.h"
#include "planner/operator/logical_plan.h"

namespace lbug {
namespace optimizer {

/* When UNWIND is followed by MERGE, duplicate values in the UNWIND list can cause
 * issues because the MERGE's HASH_JOIN doesn't see newly created nodes within the same batch.
 * This optimizer inserts a UNWIND_DEDUP operator to deduplicate MERGE input rows.
 *
 * E.g., UNWIND [1, 1, 2] AS x MERGE (a:A {val: x})
 * Before: UNWIND -> HASH_JOIN (for optional match) -> MERGE
 * After:  UNWIND -> HASH_JOIN (for optional match) -> UNWIND_DEDUP -> MERGE
 */
class UnwindDedupOptimizer : public LogicalOperatorVisitor {
public:
    void rewrite(planner::LogicalPlan* plan);

private:
    std::shared_ptr<planner::LogicalOperator> visitOperator(
        const std::shared_ptr<planner::LogicalOperator>& op, bool isRoot = false);

    std::shared_ptr<planner::LogicalOperator> visitMergeReplace(
        std::shared_ptr<planner::LogicalOperator> op) override;

    bool canRewriteCurrentMerge = false;
};

} // namespace optimizer
} // namespace lbug

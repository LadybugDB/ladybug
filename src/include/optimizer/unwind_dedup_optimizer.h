#pragma once

#include "logical_operator_visitor.h"
#include "planner/operator/logical_plan.h"

namespace lbug {
namespace optimizer {

/* When UNWIND is followed by MERGE, this optimizer inserts an UNWIND_DEDUP operator so MERGE sees
 * each key once. MERGE itself creates one pattern per key, so the dedup applies only where MERGE
 * would drop the repeated created rows anyway: a node MERGE whose rows carry nothing besides the
 * keys, and a relationship MERGE at the plan root, where no later clause reads the rows.
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

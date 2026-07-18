#pragma once

#include "logical_operator_visitor.h"
#include "planner/operator/logical_plan.h"

namespace lbug {
namespace optimizer {

/**
 * Boolean constant folding pass with bottom-up tree propagation.
 *
 * Folded subtrees bubble up so parent operators can apply further
 * simplifications.  For example an OR whose left leg folds to false
 * collapses to the right leg alone.
 *
 * Rules (applied recursively bottom-up):
 *
 *   NOT(NOT(x))                      -> x
 *   NOT(true) / NOT(false)           -> false / true
 *   AND(false, ...)                  -> false
 *   AND(p, NOT(p))                   -> false                (contradiction)
 *   AND(IS_NULL(x), IS_NOT_NULL(x))  -> false
 *   AND(true, x)                     -> x                    (identity)
 *   AND() / all-true conjuncts       -> true
 *   OR(true, ...)                    -> true
 *   OR(p, NOT(p))                    -> true                 (tautology)
 *   OR(IS_NULL(x), IS_NOT_NULL(x))   -> true
 *   OR(false, x)                     -> x                    (identity)
 *
 * When the predicate folds to false/null the FILTER operator is replaced
 * with an EmptyResult node (avoiding an unnecessary scan).  When it folds
 * to a non-literal simplification the FILTER predicate is replaced.
 */
class BoolFoldingOptimizer : public LogicalOperatorVisitor {
public:
    void rewrite(planner::LogicalPlan* plan);

private:
    std::shared_ptr<planner::LogicalOperator> visitOperator(
        const std::shared_ptr<planner::LogicalOperator>& op);

    std::shared_ptr<planner::LogicalOperator> visitFilterReplace(
        std::shared_ptr<planner::LogicalOperator> op) override;
};

} // namespace optimizer
} // namespace lbug

#pragma once

#include "planner/operator/logical_plan.h"

namespace lbug {
namespace optimizer {

class LimitPushDownOptimizer {
public:
    void rewrite(planner::LogicalPlan* plan);

private:
    struct LimitDemand {
        common::offset_t skip = 0;
        common::offset_t limit = common::INVALID_LIMIT;
        bool crossedBoundary = false;

        bool hasFiniteCap() const {
            return limit != common::INVALID_LIMIT && skip < common::INVALID_LIMIT - limit;
        }

        common::offset_t totalRowsNeeded() const { return skip + limit; }

        LimitDemand asCrossedBoundaryCap() const {
            if (!hasFiniteCap()) {
                return {};
            }
            return LimitDemand{0, totalRowsNeeded(), true};
        }
    };

    // Returns true when `op` is guaranteed to emit at most `demand.totalRowsNeeded()` rows
    // (either natively via early termination, or via a LogicalLimit inside its subtree).
    bool visitOperator(planner::LogicalOperator* op, const LimitDemand& demand);
};

} // namespace optimizer
} // namespace lbug

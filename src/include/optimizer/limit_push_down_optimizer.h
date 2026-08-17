#pragma once

#include "planner/operator/logical_plan.h"

namespace lbug {
namespace optimizer {

class LimitPushDownOptimizer {
public:
    LimitPushDownOptimizer() : skipNumber{0}, limitNumber{common::INVALID_LIMIT} {}

    // Behavior toggle for FILTER handling. When true (default), a FILTER is a hard barrier: the
    // push-down descent stops there, so nothing below the filter is ever capped. When false, the
    // original behavior is restored -- the descent continues past FILTER while only suppressing
    // the recursive-extend hash-join cap. Flip this to go back to the prior behavior without
    // removing code.
    explicit LimitPushDownOptimizer(bool treatFilterAsBarrier)
        : skipNumber{0}, limitNumber{common::INVALID_LIMIT},
          treatFilterAsBarrier{treatFilterAsBarrier} {}

    void rewrite(planner::LogicalPlan* plan);

private:
    void visitOperator(planner::LogicalOperator* op, bool canPushLimitToHashJoin = true);

private:
    common::offset_t skipNumber;
    common::offset_t limitNumber;
    const bool treatFilterAsBarrier = true;
};

} // namespace optimizer
} // namespace lbug

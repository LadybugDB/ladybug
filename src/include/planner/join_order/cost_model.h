#pragma once

#include "planner/operator/logical_plan.h"

namespace lbug {
namespace planner {

class CostModel {
public:
    static uint64_t computeExtendCost(const LogicalPlan& childPlan,
        cardinality_t estimatedOutputCardinality);
    static uint64_t computeHashJoinCost(const std::vector<binder::expression_pair>& joinConditions,
        const LogicalPlan& probe, const LogicalPlan& build);
    static uint64_t computeHashJoinCost(const binder::expression_vector& joinNodeIDs,
        const LogicalPlan& probe, const LogicalPlan& build);
    static uint64_t computeHashJoinCost(const binder::expression_vector& joinNodeIDs,
        const LogicalPlan& probe, const LogicalPlan& build,
        cardinality_t estimatedOutputCardinality);
    static uint64_t computeMarkJoinCost(const std::vector<binder::expression_pair>& joinConditions,
        const LogicalPlan& probe, const LogicalPlan& build);
    static uint64_t computeMarkJoinCost(const binder::expression_vector& joinNodeIDs,
        const LogicalPlan& probe, const LogicalPlan& build);
    static uint64_t computeIntersectCost(const LogicalPlan& probePlan,
        const std::vector<LogicalPlan>& buildPlans);
    // Lookup join: one PK probe/payload fetch per outer row on top of the outer plan.
    // Like the INL-join precedent, this only pays off for small outer cardinalities,
    // which callers enforce via PlannerKnobs::LOOKUP_JOIN_MAX_OUTER_CARDINALITY.
    static uint64_t computeLookupJoinCost(const LogicalPlan& outerPlan,
        cardinality_t outerCardinality);
};

} // namespace planner
} // namespace lbug

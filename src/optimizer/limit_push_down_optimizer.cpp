#include "optimizer/limit_push_down_optimizer.h"

#include <algorithm>
#include <string>

#include "binder/expression/expression_util.h"
#include "binder/expression/literal_expression.h"
#include "common/types/value/value.h"
#include "planner/operator/extend/logical_recursive_extend.h"
#include "planner/operator/logical_accumulate.h"
#include "planner/operator/logical_distinct.h"
#include "planner/operator/logical_hash_join.h"
#include "planner/operator/logical_limit.h"
#include "planner/operator/logical_multiplcity_reducer.h"
#include "planner/operator/logical_order_by.h"
#include "planner/operator/logical_table_function_call.h"
#include "planner/operator/operator_properties.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace optimizer {

namespace {

std::shared_ptr<Expression> createLiteralLimitExpr(offset_t cap) {
    return std::make_shared<LiteralExpression>(Value(static_cast<uint64_t>(cap)),
        "_pushed_limit_" + std::to_string(cap));
}

// Wrap `child` in MULTIPLICITY_REDUCER -> LIMIT(cap), preserving the planner invariant that
// every LogicalLimit sits on a LogicalMultiplicityReducer (relied on by TopKOptimizer and
// FactorizationRewriter). Skips wrapping if `child` is already an equivalent or tighter LIMIT.
std::shared_ptr<LogicalOperator> wrapWithLimitCap(std::shared_ptr<LogicalOperator> child,
    offset_t cap) {
    if (child->getOperatorType() == LogicalOperatorType::LIMIT) {
        const auto& existing = child->constCast<LogicalLimit>();
        if (!existing.hasSkipNum() && existing.hasLimitNum() &&
            ExpressionUtil::canEvaluateAsLiteral(*existing.getLimitNum()) &&
            ExpressionUtil::evaluateAsSkipLimit(*existing.getLimitNum()) <= cap) {
            return child;
        }
    }
    const auto childCard = child->getCardinality();
    auto reducer = std::make_shared<LogicalMultiplicityReducer>(std::move(child));
    reducer->computeFlatSchema();
    reducer->setCardinality(childCard);
    auto limitOp =
        std::make_shared<LogicalLimit>(nullptr, createLiteralLimitExpr(cap), std::move(reducer));
    limitOp->computeFlatSchema();
    limitOp->setCardinality(std::min<cardinality_t>(childCard, cap));
    return limitOp;
}

} // namespace

void LimitPushDownOptimizer::rewrite(LogicalPlan* plan) {
    visitOperator(plan->getLastOperator().get(), LimitDemand{});
}

bool LimitPushDownOptimizer::visitOperator(LogicalOperator* op, const LimitDemand& demand) {
    const auto sem = analyzeOperator(*op);
    // Exhaustive over LimitPushDownAction (no default: label).
    switch (sem.limitAction) {
    case LimitPushDownAction::STOP: {
        // Hard barrier for the incoming demand, but keep traversing children so inner LIMITs
        // deeper in the plan still optimize independently.
        for (auto i = 0u; i < op->getNumChildren(); ++i) {
            visitOperator(op->getChild(i).get(), LimitDemand{});
        }
        return false;
    }
    case LimitPushDownAction::COMBINE_LIMIT: {
        auto& limit = op->constCast<LogicalLimit>();
        auto outerCap = INVALID_LIMIT;
        if (demand.hasFiniteCap()) {
            outerCap = demand.totalRowsNeeded();
        }
        LimitDemand next;
        next.skip = 0;
        next.limit = outerCap;
        if (limit.hasLimitNum() && ExpressionUtil::canEvaluateAsLiteral(*limit.getLimitNum())) {
            next.limit =
                std::min(next.limit, ExpressionUtil::evaluateAsSkipLimit(*limit.getLimitNum()));
        }
        if (limit.hasSkipNum()) {
            if (ExpressionUtil::canEvaluateAsLiteral(*limit.getSkipNum())) {
                next.skip = ExpressionUtil::evaluateAsSkipLimit(*limit.getSkipNum());
            } else {
                next.limit = INVALID_LIMIT;
            }
        }
        next.crossedBoundary = false;
        visitOperator(limit.getChild(0).get(), next);
        return demand.hasFiniteCap() && next.hasFiniteCap() && !limit.hasSkipNum() &&
               next.totalRowsNeeded() <= demand.totalRowsNeeded();
    }
    case LimitPushDownAction::PASS_THROUGH_CHILD_0: {
        if (op->getNumChildren() == 0) {
            return false;
        }
        // Visit any auxiliary children (e.g. PATH_PROPERTY_PROBE's node/rel property scans)
        // independently without forwarding row demand.
        for (auto i = 1u; i < op->getNumChildren(); ++i) {
            visitOperator(op->getChild(i).get(), LimitDemand{});
        }
        const auto childCapped = visitOperator(op->getChild(0).get(), demand);
        return childCapped && sem.child0Cardinality == RowCardinalityEffect::EXACT_ONE;
    }
    case LimitPushDownAction::CAP_AND_PASS_THROUGH_CHILD_0: {
        // ACCUMULATE materializes its entire child into a FactorizedTable before downstream sees
        // any rows. Capping child 0 stops ResultCollector at `cap` rows while preserving the
        // ACCUMULATE node itself (and any correlated EXPRESSIONS_SCAN pointer to it).
        auto& acc = op->cast<LogicalAccumulate>();
        auto child = acc.getChild(0);
        if (!demand.hasFiniteCap() || subtreeHasSideEffects(*child)) {
            visitOperator(child.get(), LimitDemand{});
            return false;
        }
        const auto cap = demand.totalRowsNeeded();
        // ACCUMULATE(OPTIONAL_) synthesizes 1 NULL row on empty input, so capping to 0 would turn
        // a non-empty input into a spurious NULL row. Require cap >= 1 for OPTIONAL_.
        if (isOptionalAccumulate(acc.getAccumulateType()) && cap == 0) {
            visitOperator(child.get(), LimitDemand{});
            return false;
        }
        // Let native-limit / inner-limit children absorb first (e.g. DISTINCT, TABLE_FUNCTION_CALL,
        // or deeper ACCUMULATEs across joins).
        const auto childDemand = demand.crossedBoundary ? demand.asCrossedBoundaryCap() : demand;
        const bool childCapped = visitOperator(child.get(), childDemand);
        if (!childCapped) {
            // Child did not guarantee a cap of <= `cap` rows (e.g. FILTER or LEFT join fan-out).
            // Install a physical cap directly below ACCUMULATE so ResultCollector terminates early.
            acc.setChild(0, wrapWithLimitCap(std::move(child), cap));
        }
        acc.setCardinality(std::min<cardinality_t>(acc.getCardinality(), cap));
        return true;
    }
    case LimitPushDownAction::ABSORB_NATIVE: {
        switch (op->getOperatorType()) {
        case LogicalOperatorType::TABLE_FUNCTION_CALL: {
            if (!demand.hasFiniteCap()) {
                return false;
            }
            auto& tableFuncCall = op->cast<LogicalTableFunctionCall>();
            const auto cap = demand.totalRowsNeeded();
            tableFuncCall.setLimitNum(cap);
            tableFuncCall.setCardinality(
                std::min<cardinality_t>(tableFuncCall.getCardinality(), cap));
            return true;
        }
        case LogicalOperatorType::DISTINCT: {
            // Visit child independently so nested LIMITs below DISTINCT still optimize.
            visitOperator(op->getChild(0).get(), LimitDemand{});
            if (!demand.hasFiniteCap()) {
                return false;
            }
            auto& distinctOp = op->cast<LogicalDistinct>();
            distinctOp.setLimitNum(demand.limit);
            distinctOp.setSkipNum(demand.skip);
            distinctOp.setCardinality(
                std::min<cardinality_t>(distinctOp.getCardinality(), demand.totalRowsNeeded()));
            return true;
        }
        case LogicalOperatorType::RECURSIVE_EXTEND: {
            for (auto i = 0u; i < op->getNumChildren(); ++i) {
                visitOperator(op->getChild(i).get(), LimitDemand{});
            }
            if (!demand.hasFiniteCap()) {
                return false;
            }
            auto& extend = op->cast<LogicalRecursiveExtend>();
            const auto cap = demand.totalRowsNeeded();
            extend.setLimitNum(cap);
            extend.setCardinality(std::min<cardinality_t>(extend.getCardinality(), cap));
            return true;
        }
        case LogicalOperatorType::ORDER_BY: {
            // Cannot push LIMIT below ORDER_BY; visit child independently.
            visitOperator(op->getChild(0).get(), LimitDemand{});
            auto& orderBy = op->cast<LogicalOrderBy>();
            // When demand arrived across a join/accumulate/union boundary, fuse the cap directly
            // into ORDER_BY so it executes as TOP_K(cap). Adjacent LIMIT -> ORDER_BY (without a
            // crossed boundary) is left to TopKOptimizer, which also folds SKIP.
            if (demand.hasFiniteCap() && demand.crossedBoundary && !orderBy.hasLimitNum() &&
                !orderBy.hasSkipNum() && !subtreeHasSideEffects(*op)) {
                const auto cap = demand.totalRowsNeeded();
                orderBy.setLimitNum(createLiteralLimitExpr(cap));
                orderBy.setCardinality(std::min<cardinality_t>(orderBy.getCardinality(), cap));
                return true;
            }
            return false;
        }
        default:
            return false;
        }
    }
    case LimitPushDownAction::PUSH_TO_PROBE_CHILD: {
        // Visit build side (child 1) independently so any LIMIT inside it still optimizes.
        visitOperator(op->getChild(1).get(), LimitDemand{});
        auto probeChild = op->getChild(0);
        if (!demand.hasFiniteCap() || subtreeHasSideEffects(*probeChild)) {
            visitOperator(probeChild.get(), LimitDemand{});
            return false;
        }
        const auto cap = demand.totalRowsNeeded();
        const auto probeDemand = demand.asCrossedBoundaryCap();
        const bool probeCapped = visitOperator(probeChild.get(), probeDemand);
        if (!probeCapped) {
            // Materialize the cap on the streaming probe side so its cardinality reflects `cap`
            // (enabling probe-to-build SIP in HashJoinSIPOptimizer without a separate limit pass).
            op->setChild(0, wrapWithLimitCap(std::move(probeChild), cap));
        }
        // Only an EXACT_ONE join (MARK, COUNT, or provably 1:1 INNER) guarantees that capping the
        // probe at `cap` also caps the join's output at `cap`. A LEFT join (AT_LEAST_ONE) can fan
        // out each probe row across multiple build matches, so any enclosing ACCUMULATE must still
        // install its own cap above this join.
        if (sem.child0Cardinality == RowCardinalityEffect::EXACT_ONE) {
            op->setCardinality(std::min<cardinality_t>(op->getCardinality(), cap));
            return true;
        }
        return false;
    }
    case LimitPushDownAction::PUSH_TO_ALL_CHILDREN: {
        // Guard cap >= 1 so OPTIONAL cross products never turn a non-empty build into an empty
        // one that synthesizes a NULL row.
        const bool canPush = demand.hasFiniteCap() && demand.totalRowsNeeded() >= 1;
        if (!canPush) {
            for (auto i = 0u; i < op->getNumChildren(); ++i) {
                visitOperator(op->getChild(i).get(), LimitDemand{});
            }
            return false;
        }
        const auto cap = demand.totalRowsNeeded();
        const auto childDemand = demand.asCrossedBoundaryCap();
        const bool isUnion = op->getOperatorType() == LogicalOperatorType::UNION_ALL;
        for (auto i = 0u; i < op->getNumChildren(); ++i) {
            auto child = op->getChild(i);
            if (subtreeHasSideEffects(*child)) {
                visitOperator(child.get(), LimitDemand{});
                continue;
            }
            const bool childCapped = visitOperator(child.get(), childDemand);
            // Every UNION_ALL branch and the build side (child 1) of CROSS_PRODUCT materialize
            // into a ResultCollector, so cap them explicitly when not already capped.
            if (!childCapped && (isUnion || i == 1)) {
                op->setChild(i, wrapWithLimitCap(std::move(child), cap));
            }
        }
        return false;
    }
    }
    return false;
}

} // namespace optimizer
} // namespace lbug

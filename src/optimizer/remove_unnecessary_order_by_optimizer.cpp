#include "optimizer/remove_unnecessary_order_by_optimizer.h"

#include "binder/expression/aggregate_function_expression.h"
#include "function/aggregate/count_star.h"
#include "planner/operator/logical_aggregate.h"
#include "planner/operator/logical_limit.h"
#include "planner/operator/logical_multiplcity_reducer.h"
#include "planner/operator/logical_order_by.h"
#include "planner/operator/logical_projection.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace optimizer {

static bool countsRowsOnly(const LogicalAggregate& aggregate) {
    for (auto& expr : aggregate.getAggregates()) {
        if (expr->expressionType != ExpressionType::AGGREGATE_FUNCTION ||
            expr->constCast<AggregateFunctionExpression>().getFunction().name !=
                function::CountStarFunction::name) {
            return false;
        }
    }
    return true;
}

void RemoveUnnecessaryOrderByOptimizer::rewrite(LogicalPlan* plan) {
    plan->setLastOperator(visitOperator(plan->getLastOperator()));
}

std::shared_ptr<planner::LogicalOperator> RemoveUnnecessaryOrderByOptimizer::visitOperator(
    std::shared_ptr<planner::LogicalOperator> op) {
    // bottom-up traversal: rewrite children first
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        op->setChild(i, visitOperator(op->getChild(i)));
    }
    auto result = tryRemoveOrderByFromAggregate(std::move(op));
    // This optimizer runs after FactorizationRewriter, so the plan is in
    // factorized form. Recompute the factorized schema (computeFlatSchema would
    // be incorrect here and would also throw on FLATTEN nodes).
    result->computeFactorizedSchema();
    return result;
}

std::shared_ptr<planner::LogicalOperator>
RemoveUnnecessaryOrderByOptimizer::tryRemoveOrderByFromAggregate(
    std::shared_ptr<planner::LogicalOperator> op) {
    if (op->getOperatorType() != LogicalOperatorType::AGGREGATE) {
        return op;
    }
    auto& aggregate = op->cast<LogicalAggregate>();

    // Only optimize aggregates without GROUP BY keys (e.g. COUNT(*))
    if (aggregate.hasKeys()) {
        return op;
    }
    // ORDER BY in WITH requires SKIP or LIMIT, so the ORDER BY decides which rows reach the
    // aggregate. COUNT(*) is the only aggregate that doesn't depend on which rows those are,
    // unless a FILTER sits in between (stripOrderByBelow stops there).
    if (!countsRowsOnly(aggregate)) {
        return op;
    }

    // Walk the child chain looking for ORDER BY to remove. If an ORDER BY is
    // found (possibly through passthrough operators), it is replaced with its
    // child, optionally wrapped in a LIMIT when the ORDER BY had absorbed one.
    bool changed = false;
    auto newChild = stripOrderByBelow(op->getChild(0), changed);
    if (changed) {
        op->setChild(0, std::move(newChild));
    }
    return op;
}

std::shared_ptr<planner::LogicalOperator> RemoveUnnecessaryOrderByOptimizer::stripOrderByBelow(
    std::shared_ptr<planner::LogicalOperator> op, bool& changed) {
    switch (op->getOperatorType()) {
    case LogicalOperatorType::ORDER_BY: {
        // `op` is the ORDER BY to remove. Replace it with its child, preserving
        // any absorbed LIMIT (TOP_K) as a standalone LIMIT so the row count
        // is unaffected.
        auto& orderBy = op->cast<LogicalOrderBy>();
        auto grandChild = orderBy.getChild(0);
        std::shared_ptr<planner::LogicalOperator> replacement;
        if (orderBy.hasLimitNum()) {
            auto limitExpr = orderBy.getLimitNum();
            auto skipExpr = orderBy.hasSkipNum() ? orderBy.getSkipNum() : nullptr;
            // The sort expanded each row's multiplicity; a LIMIT counts a flat row with
            // multiplicity m as m rows and cannot cut it, so reduce it first, as the planner
            // does before every SKIP/LIMIT.
            auto reducer = std::make_shared<LogicalMultiplicityReducer>(grandChild);
            reducer->computeFactorizedSchema();
            auto limitOp = std::make_shared<LogicalLimit>(skipExpr, limitExpr, reducer);
            // The newly created Limit has no schema yet; compute it now so that
            // the parent operator can safely copy it.
            limitOp->computeFactorizedSchema();
            replacement = std::move(limitOp);
        } else {
            replacement = grandChild;
        }
        changed = true;
        return replacement;
    }
    case LogicalOperatorType::PROJECTION:
    case LogicalOperatorType::MULTIPLICITY_REDUCER:
    case LogicalOperatorType::LIMIT: {
        // Passthrough operator: recurse into its single child. If a removal
        // happened deeper, rewire the child and recompute this operator's schema.
        if (op->getNumChildren() != 1) {
            return op;
        }
        bool childChanged = false;
        auto newChild = stripOrderByBelow(op->getChild(0), childChanged);
        if (childChanged) {
            op->setChild(0, std::move(newChild));
            op->computeFactorizedSchema();
            changed = true;
        }
        return op;
    }
    // COUNT(*) above a FILTER counts the kept rows that pass it, which
    // depends on which rows the ORDER BY kept.
    case LogicalOperatorType::FILTER:
    default:
        // Don't traverse into other operators (joins, scans, extends, ...);
        // an ORDER BY buried below them is not safe to reach through here.
        return op;
    }
}

} // namespace optimizer
} // namespace lbug

#include "optimizer/limit_push_down_optimizer.h"

#include "binder/expression/expression_util.h"
#include "planner/operator/extend/logical_recursive_extend.h"
#include "planner/operator/logical_distinct.h"
#include "planner/operator/logical_hash_join.h"
#include "planner/operator/logical_limit.h"
#include "planner/operator/logical_table_function_call.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace optimizer {

// A LIMIT/SKIP cap may only be pushed through operators that never reduce the number of rows
// they emit relative to their input (i.e. 1:1 or 1:many operators). Any operator that can
// discard rows -- FILTER, DISTINCT, joins, aggregates, UNWIND of empty lists, etc. -- would
// leave the parent LIMIT with too few surviving rows if a static cap were placed below it, so
// those are hard barriers. The cap is instead applied directly to operators that support early
// termination (TABLE_FUNCTION_CALL, DISTINCT, and the recursive extend behind its 1:1
// property-probe join, see visitOperator below).
static bool isPushDownSupported(planner::LogicalOperator* op) {
    switch (op->getOperatorType()) {
    case LogicalOperatorType::MULTIPLICITY_REDUCER:
    case LogicalOperatorType::EXPLAIN:
    case LogicalOperatorType::ACCUMULATE:
    case LogicalOperatorType::PROJECTION:
        return true;
    default:
        return false;
    }
}

void LimitPushDownOptimizer::rewrite(LogicalPlan* plan) {
    visitOperator(plan->getLastOperator().get());
}

void LimitPushDownOptimizer::visitOperator(planner::LogicalOperator* op) {
    switch (op->getOperatorType()) {
    case LogicalOperatorType::LIMIT: {
        auto& limit = op->constCast<LogicalLimit>();
        if (limit.hasSkipNum() && ExpressionUtil::canEvaluateAsLiteral(*limit.getSkipNum())) {
            skipNumber = ExpressionUtil::evaluateAsSkipLimit(*limit.getSkipNum());
        }
        if (limit.hasLimitNum() && ExpressionUtil::canEvaluateAsLiteral(*limit.getLimitNum())) {
            limitNumber = ExpressionUtil::evaluateAsSkipLimit(*limit.getLimitNum());
        }
        visitOperator(limit.getChild(0).get());
        return;
    }
    case LogicalOperatorType::TABLE_FUNCTION_CALL: {
        // SKIP without LIMIT has no finite row demand and skip + limit may overflow.
        if (limitNumber == INVALID_LIMIT || skipNumber >= INVALID_LIMIT - limitNumber) {
            return;
        }
        auto& tableFuncCall = op->cast<LogicalTableFunctionCall>();
        if (tableFuncCall.getTableFunc().supportsPushDownFunc()) {
            tableFuncCall.setLimitNum(skipNumber + limitNumber);
        }
        return;
    }
    case LogicalOperatorType::DISTINCT: {
        // The physical DISTINCT is capped at skip + limit distinct groups, after which the
        // remaining LIMIT/SKIP operator on top selects the requested rows. SKIP without LIMIT
        // must not cap DISTINCT, and skip + limit must not overflow.
        if (limitNumber == INVALID_LIMIT || skipNumber >= INVALID_LIMIT - limitNumber) {
            return;
        }
        auto& distinctOp = op->cast<LogicalDistinct>();
        distinctOp.setLimitNum(limitNumber);
        distinctOp.setSkipNum(skipNumber);
        return;
    }
    case LogicalOperatorType::HASH_JOIN: {
        if (limitNumber == INVALID_LIMIT) {
            return;
        }
        auto& hashJoin = op->cast<LogicalHashJoin>();
        // The recursive-extend join created by Planner::appendRecursiveExtend is a 1:1 INNER
        // join that reads the bound node properties back after path expansion. Only that direct
        // shape is safe: nested joins, filtering joins, and multiplicative joins must remain
        // barriers because their first N probe rows may produce fewer than N final rows.
        if (hashJoin.getJoinType() != JoinType::INNER || hashJoin.requireFlatProbeKeys()) {
            return;
        }
        if (op->getChild(0)->getOperatorType() != LogicalOperatorType::PATH_PROPERTY_PROBE ||
            op->getChild(0)->getChild(0)->getOperatorType() !=
                LogicalOperatorType::RECURSIVE_EXTEND ||
            skipNumber >= INVALID_LIMIT - limitNumber) {
            return;
        }
        auto& extend = op->getChild(0)->getChild(0)->cast<LogicalRecursiveExtend>();
        extend.setLimitNum(skipNumber + limitNumber);
        return;
    }
    case LogicalOperatorType::UNION_ALL: {
        for (auto i = 0u; i < op->getNumChildren(); ++i) {
            auto optimizer = LimitPushDownOptimizer();
            optimizer.visitOperator(op->getChild(i).get());
        }
        return;
    }
    default: {
        // Only row-count-preserving operators let the cap pass through; FILTER and every
        // other operator that can alter the number of rows is a barrier.
        if (isPushDownSupported(op)) {
            visitOperator(op->getChild(0).get());
        }
        return;
    }
    }
}

} // namespace optimizer
} // namespace lbug

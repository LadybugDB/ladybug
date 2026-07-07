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

void LimitPushDownOptimizer::rewrite(LogicalPlan* plan) {
    visitOperator(plan->getLastOperator().get());
}

void LimitPushDownOptimizer::visitOperator(planner::LogicalOperator* op,
    bool canPushLimitToHashJoin) {
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
    case LogicalOperatorType::MULTIPLICITY_REDUCER:
    case LogicalOperatorType::EXPLAIN:
    case LogicalOperatorType::ACCUMULATE:
    case LogicalOperatorType::PROJECTION: {
        visitOperator(op->getChild(0).get(), canPushLimitToHashJoin);
        return;
    }
    case LogicalOperatorType::FILTER: {
        // A filter between LIMIT and HASH_JOIN can reject rows after the join. A static
        // pre-join cap could then leave the parent LIMIT with too few surviving rows.
        visitOperator(op->getChild(0).get(), false);
        return;
    }
    case LogicalOperatorType::TABLE_FUNCTION_CALL: {
        if (limitNumber == INVALID_LIMIT && skipNumber == 0) {
            return;
        }
        auto& tableFuncCall = op->cast<LogicalTableFunctionCall>();
        if (tableFuncCall.getTableFunc().supportsPushDownFunc()) {
            tableFuncCall.setLimitNum(skipNumber + limitNumber);
        }
        return;
    }
    case LogicalOperatorType::DISTINCT: {
        if (limitNumber == INVALID_LIMIT && skipNumber == 0) {
            return;
        }
        auto& distinctOp = op->cast<LogicalDistinct>();
        distinctOp.setLimitNum(limitNumber);
        distinctOp.setSkipNum(skipNumber);
        return;
    }
    case LogicalOperatorType::HASH_JOIN: {
        if (limitNumber == INVALID_LIMIT || !canPushLimitToHashJoin) {
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
                LogicalOperatorType::RECURSIVE_EXTEND || skipNumber > INVALID_LIMIT - limitNumber) {
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
    default:
        return;
    }
}

} // namespace optimizer
} // namespace lbug

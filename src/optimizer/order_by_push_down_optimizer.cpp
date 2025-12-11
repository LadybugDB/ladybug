#include "optimizer/order_by_push_down_optimizer.h"

#include "binder/expression/expression.h"
#include "binder/expression/variable_expression.h"
#include "binder/expression/expression_util.h"
#include "common/exception/runtime.h"
#include "planner/operator/logical_order_by.h"
#include "planner/operator/logical_table_function_call.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace optimizer {

void OrderByPushDownOptimizer::rewrite(LogicalPlan* plan) {
    visitOperator(plan->getLastOperator().get());
}

void OrderByPushDownOptimizer::visitOperator(planner::LogicalOperator* op) {
    switch (op->getOperatorType()) {
    case LogicalOperatorType::ORDER_BY: {
        auto& orderBy = op->constCast<LogicalOrderBy>();
        orderByString = buildOrderByString(orderBy.getExpressionsToOrderBy(), orderBy.getIsAscOrders());
        visitOperator(orderBy.getChild(0).get());
        return;
    }
    case LogicalOperatorType::MULTIPLICITY_REDUCER:
    case LogicalOperatorType::EXPLAIN:
    case LogicalOperatorType::ACCUMULATE:
    case LogicalOperatorType::FILTER:
    case LogicalOperatorType::PROJECTION: {
        visitOperator(op->getChild(0).get());
        return;
    }
    case LogicalOperatorType::TABLE_FUNCTION_CALL: {
        if (!orderByString.empty()) {
            auto& tableFuncCall = op->cast<LogicalTableFunctionCall>();
            tableFuncCall.setOrderBy(orderByString);
        }
        return;
    }
    default:
        return;
    }
}

std::string OrderByPushDownOptimizer::buildOrderByString(
    const binder::expression_vector& expressions, const std::vector<bool>& isAscOrders) {
    if (expressions.empty()) {
        return "";
    }
    std::string result = " ORDER BY ";
    for (size_t i = 0; i < expressions.size(); ++i) {
        if (i > 0) {
            result += ", ";
        }
        auto& expr = expressions[i];
        if (expr->expressionType == common::ExpressionType::VARIABLE) {
            auto& var = expr->constCast<binder::VariableExpression>();
            result += var.getVariableName();
        } else {
            // For now, assume variables. Could extend for more complex expressions.
            throw RuntimeException("ORDER BY push down only supports variable expressions.");
        }
        result += isAscOrders[i] ? " ASC" : " DESC";
    }
    return result;
}

} // namespace optimizer
} // namespace lbug
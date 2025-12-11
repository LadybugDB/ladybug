#include "optimizer/order_by_push_down_optimizer.h"

#include "binder/expression/expression.h"
#include "binder/expression/expression_util.h"
#include "binder/expression/property_expression.h"
#include "binder/expression/variable_expression.h"
#include "common/exception/runtime.h"
#include "planner/operator/logical_order_by.h"
#include "planner/operator/logical_table_function_call.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace optimizer {

void OrderByPushDownOptimizer::rewrite(LogicalPlan* plan) {
    plan->setLastOperator(visitOperator(plan->getLastOperator()));
}

std::shared_ptr<LogicalOperator> OrderByPushDownOptimizer::visitOperator(
    std::shared_ptr<LogicalOperator> op, std::string currentOrderBy) {
    switch (op->getOperatorType()) {
    case LogicalOperatorType::ORDER_BY: {
        auto& orderBy = op->constCast<LogicalOrderBy>();
        std::string newOrderBy = currentOrderBy;
        if (!currentOrderBy.empty()) {
            newOrderBy += ", ";
        }
        newOrderBy +=
            buildOrderByString(orderBy.getExpressionsToOrderBy(), orderBy.getIsAscOrders());
        auto newChild = visitOperator(orderBy.getChild(0), newOrderBy);
        if (newChild->getOperatorType() == LogicalOperatorType::TABLE_FUNCTION_CALL) {
            auto& tableFunc = newChild->cast<LogicalTableFunctionCall>();
            tableFunc.setOrderBy(newOrderBy);
            return newChild;
        } else {
            return std::make_shared<LogicalOrderBy>(orderBy.getExpressionsToOrderBy(),
                orderBy.getIsAscOrders(), newChild);
        }
    }
    case LogicalOperatorType::MULTIPLICITY_REDUCER:
    case LogicalOperatorType::EXPLAIN:
    case LogicalOperatorType::ACCUMULATE:
    case LogicalOperatorType::FILTER:
    case LogicalOperatorType::PROJECTION:
    case LogicalOperatorType::LIMIT: {
        for (auto i = 0u; i < op->getNumChildren(); ++i) {
            op->setChild(i, visitOperator(op->getChild(i), currentOrderBy));
        }
        return op;
    }
    case LogicalOperatorType::TABLE_FUNCTION_CALL: {
        if (!currentOrderBy.empty()) {
            auto& tableFunc = op->cast<LogicalTableFunctionCall>();
            tableFunc.setOrderBy(currentOrderBy);
        }
        return op;
    }
    default:
        return op;
    }
}

std::string OrderByPushDownOptimizer::buildOrderByString(
    const binder::expression_vector& expressions, const std::vector<bool>& isAscOrders) {
    if (expressions.empty()) {
        return "";
    }
    std::string result = " ORDER BY ";
    bool first = true;
    for (size_t i = 0; i < expressions.size(); ++i) {
        auto& expr = expressions[i];
        std::string colName;
        if (expr->expressionType == common::ExpressionType::VARIABLE) {
            auto& var = expr->constCast<binder::VariableExpression>();
            colName = var.getVariableName();
        } else if (expr->expressionType == common::ExpressionType::PROPERTY) {
            auto& prop = expr->constCast<binder::PropertyExpression>();
            colName = prop.getPropertyName();
        } else {
            // Skip expressions that cannot be pushed down
            continue;
        }
        if (!first) {
            result += ", ";
        }
        result += colName;
        result += isAscOrders[i] ? " ASC" : " DESC";
        first = false;
    }
    if (first) {
        // No expressions could be pushed down
        return "";
    }
    return result;
}

} // namespace optimizer
} // namespace lbug
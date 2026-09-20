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

// This ensures that ORDER BY can be pushed down only through operators that support it.
// It should not be pushed down for things like RECURSIVE_EXTEND etc.
bool isPushDownSupported(LogicalOperator* op) {
    switch (op->getOperatorType()) {
    case LogicalOperatorType::TABLE_FUNCTION_CALL: {
        return op->cast<LogicalTableFunctionCall>().getTableFunc().supportsPushDownFunc();
    }
    case LogicalOperatorType::MULTIPLICITY_REDUCER:
    case LogicalOperatorType::EXPLAIN:
    case LogicalOperatorType::ACCUMULATE:
    case LogicalOperatorType::FILTER:
    case LogicalOperatorType::PROJECTION:
    case LogicalOperatorType::LIMIT: {
        if (op->getNumChildren() == 0) {
            return false;
        }
        return isPushDownSupported(op->getChild(0).get());
    }
    default:
        return false;
    }
}

void OrderByPushDownOptimizer::rewrite(LogicalPlan* plan) {
    plan->setLastOperator(visitOperator(plan->getLastOperator()));
}

static const lbug::function::TableFuncBindData* findPushdownScanTarget(
    planner::LogicalOperator* op);

std::shared_ptr<LogicalOperator> OrderByPushDownOptimizer::visitOperator(
    std::shared_ptr<LogicalOperator> op, std::string currentOrderBy) {
    switch (op->getOperatorType()) {
    case LogicalOperatorType::ORDER_BY: {
        auto& orderBy = op->constCast<LogicalOrderBy>();
        std::string newOrderBy = currentOrderBy;
        if (!currentOrderBy.empty()) {
            newOrderBy += ", ";
        }
        // Resolve sort keys against the pushdown scan they flow into so
        // multi-table (joined) scans sort by "table.column" references.
        auto* scanTarget = findPushdownScanTarget(orderBy.getChild(0).get());
        newOrderBy += buildOrderByString(orderBy.getExpressionsToOrderBy(),
            orderBy.getIsAscOrders(), scanTarget);
        auto newChild = visitOperator(orderBy.getChild(0), newOrderBy);
        // Only drop the ORDER BY when sort keys were actually pushed: an
        // untranslatable key (e.g. an aggregate call over a pushed scan)
        // leaves newOrderBy empty and must stay local, otherwise the sort is
        // silently lost.
        if (!newOrderBy.empty() && isPushDownSupported(newChild.get())) {
            return newChild;
        }
        return std::make_shared<LogicalOrderBy>(orderBy.getExpressionsToOrderBy(),
            orderBy.getIsAscOrders(), newChild);
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
            if (tableFunc.getTableFunc().supportsPushDownFunc()) {
                tableFunc.setOrderBy(currentOrderBy);
            }
        }
        return op;
    }
    default:
        return op;
    }
}

// Find the single pushdown scan an ORDER BY would flow into through
// pass-through operators (mirrors isPushDownSupported). Returns nullptr when
// there is no such scan.
static const function::TableFuncBindData* findPushdownScanTarget(planner::LogicalOperator* op) {
    auto current = op;
    while (current != nullptr) {
        switch (current->getOperatorType()) {
        case planner::LogicalOperatorType::TABLE_FUNCTION_CALL: {
            auto& tableFunc = current->constCast<planner::LogicalTableFunctionCall>();
            if (!tableFunc.getTableFunc().supportsPushDownFunc()) {
                return nullptr;
            }
            return tableFunc.getBindData();
        }
        case planner::LogicalOperatorType::MULTIPLICITY_REDUCER:
        case planner::LogicalOperatorType::EXPLAIN:
        case planner::LogicalOperatorType::ACCUMULATE:
        case planner::LogicalOperatorType::FILTER:
        case planner::LogicalOperatorType::PROJECTION:
        case planner::LogicalOperatorType::LIMIT: {
            if (current->getNumChildren() != 1) {
                return nullptr;
            }
            current = current->getChild(0).get();
            break;
        }
        default:
            return nullptr;
        }
    }
    return nullptr;
}

std::string OrderByPushDownOptimizer::buildOrderByString(
    const binder::expression_vector& expressions, const std::vector<bool>& isAscOrders) {
    return buildOrderByString(expressions, isAscOrders, nullptr);
}

std::string OrderByPushDownOptimizer::buildOrderByString(
    const binder::expression_vector& expressions, const std::vector<bool>& isAscOrders,
    const lbug::function::TableFuncBindData* target) {
    if (expressions.empty()) {
        return "";
    }
    std::string result = " ORDER BY ";
    bool first = true;
    for (size_t i = 0; i < expressions.size(); ++i) {
        auto& expr = expressions[i];
        std::string colName;
        // Prefer the scan's own output alias when the expression denotes a
        // pushed column (matched by unique name). For joined scans the alias
        // is a "table.column" reference that resolves against the pushed
        // query's range variables; for single-table scans it is the bare
        // column name, matching the historical behaviour.
        if (target != nullptr) {
            for (auto& column : target->columns) {
                if (column->getUniqueName() == expr->getUniqueName()) {
                    colName = column->getAlias();
                    break;
                }
            }
            // Internal-ID aliases ("var._ID") have no SQL counterpart in
            // the pushed query (the external ID column is selected instead);
            // the whole ORDER BY must stay local then.
            if (!colName.empty() && colName.size() >= 4 &&
                colName.compare(colName.size() - 4, 4, "._ID") == 0) {
                return "";
            }
        }
        if (colName.empty()) {
            if (expr->expressionType == common::ExpressionType::VARIABLE) {
                auto& var = expr->constCast<binder::VariableExpression>();
                colName = var.getVariableName();
            } else if (expr->expressionType == common::ExpressionType::PROPERTY) {
                auto& prop = expr->constCast<binder::PropertyExpression>();
                colName = prop.getPropertyName();
            } else {
                // One untranslatable key keeps the whole ORDER BY local:
                // pushing a prefix while dropping the operator would silently
                // lose the remaining sort keys.
                return "";
            }
        }
        if (!first) {
            result += ", ";
        }
        result += colName;
        result += isAscOrders[i] ? " ASC" : " DESC";
        first = false;
    }
    return result;
}

} // namespace optimizer
} // namespace lbug
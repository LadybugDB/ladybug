#include "optimizer/order_by_push_down_optimizer.h"

#include "binder/expression/expression.h"
#include "binder/expression/property_expression.h"
#include "binder/expression/variable_expression.h"
#include "planner/operator/logical_order_by.h"
#include "planner/operator/logical_table_function_call.h"
#include "planner/operator/operator_properties.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace optimizer {

namespace {

// Find the single pushdown scan an ORDER BY would flow into through strictly order-preserving,
// non-limiting operators (driven by OrderByPushDownAction). Returns nullptr when no such scan
// exists (e.g. a LIMIT, ACCUMULATE, join, or non-pushdown scan sits in between).
const function::TableFuncBindData* findPushdownScanTarget(const LogicalOperator* op) {
    const LogicalOperator* current = op;
    while (current != nullptr) {
        const auto sem = analyzeOperator(*current);
        // Exhaustive over OrderByPushDownAction (no default: label).
        switch (sem.orderByAction) {
        case OrderByPushDownAction::ABSORB_NATIVE: {
            if (current->getOperatorType() == LogicalOperatorType::TABLE_FUNCTION_CALL) {
                return current->constCast<LogicalTableFunctionCall>().getBindData();
            }
            return nullptr;
        }
        case OrderByPushDownAction::PASS_THROUGH_CHILD_0:
        case OrderByPushDownAction::COMBINE_ORDER_BY: {
            if (current->getNumChildren() != 1) {
                return nullptr;
            }
            current = current->getChild(0).get();
            break;
        }
        case OrderByPushDownAction::STOP:
            return nullptr;
        }
    }
    return nullptr;
}

} // namespace

void OrderByPushDownOptimizer::rewrite(LogicalPlan* plan) {
    plan->setLastOperator(visitOperator(plan->getLastOperator()));
}

std::shared_ptr<LogicalOperator> OrderByPushDownOptimizer::visitOperator(
    std::shared_ptr<LogicalOperator> op, std::string currentOrderBy) {
    const auto sem = analyzeOperator(*op);
    // Exhaustive over OrderByPushDownAction (no default: label).
    switch (sem.orderByAction) {
    case OrderByPushDownAction::COMBINE_ORDER_BY: {
        auto& orderBy = op->constCast<LogicalOrderBy>();
        std::string newOrderBy = currentOrderBy;
        if (!currentOrderBy.empty()) {
            newOrderBy += ", ";
        }
        // Resolve sort keys against the pushdown scan they flow into so
        // multi-table (joined) scans sort by "table.column" references.
        const auto* scanTarget = findPushdownScanTarget(orderBy.getChild(0).get());
        if (scanTarget != nullptr) {
            newOrderBy += buildOrderByString(orderBy.getExpressionsToOrderBy(),
                orderBy.getIsAscOrders(), scanTarget);
        } else {
            newOrderBy.clear();
        }
        auto newChild = visitOperator(orderBy.getChild(0), newOrderBy);
        // Only drop the ORDER BY when sort keys were actually pushed into a target scan.
        if (!newOrderBy.empty() && scanTarget != nullptr) {
            return newChild;
        }
        return std::make_shared<LogicalOrderBy>(orderBy.getExpressionsToOrderBy(),
            orderBy.getIsAscOrders(), std::move(newChild));
    }
    case OrderByPushDownAction::PASS_THROUGH_CHILD_0: {
        if (op->getNumChildren() > 0) {
            op->setChild(0, visitOperator(op->getChild(0), currentOrderBy));
        }
        for (auto i = 1u; i < op->getNumChildren(); ++i) {
            op->setChild(i, visitOperator(op->getChild(i), ""));
        }
        return op;
    }
    case OrderByPushDownAction::ABSORB_NATIVE: {
        if (!currentOrderBy.empty() &&
            op->getOperatorType() == LogicalOperatorType::TABLE_FUNCTION_CALL) {
            auto& tableFunc = op->cast<LogicalTableFunctionCall>();
            if (tableFunc.getTableFunc().supportsPushDownFunc()) {
                tableFunc.setOrderBy(currentOrderBy);
            }
        }
        return op;
    }
    case OrderByPushDownAction::STOP: {
        // Hard barrier for incoming sort pushdown (e.g. LIMIT, ACCUMULATE, joins, writes),
        // but still visit children independently so an ORDER BY below this barrier can push down.
        for (auto i = 0u; i < op->getNumChildren(); ++i) {
            op->setChild(i, visitOperator(op->getChild(i), ""));
        }
        return op;
    }
    }
    return op;
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

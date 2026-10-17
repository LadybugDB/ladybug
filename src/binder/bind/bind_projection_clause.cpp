#include <algorithm>
#include <unordered_set>
#include <utility>

#include "binder/binder.h"
#include "binder/expression/expression_util.h"
#include "binder/expression/lambda_expression.h"
#include "binder/expression_visitor.h"
#include "binder/query/return_with_clause/bound_return_clause.h"
#include "binder/query/return_with_clause/bound_with_clause.h"
#include "common/exception/binder.h"
#include "parser/expression/parsed_expression_visitor.h"
#include "parser/expression/parsed_property_expression.h"
#include "parser/expression/parsed_variable_expression.h"
#include "parser/query/return_with_clause/with_clause.h"
#include <format>

using namespace lbug::common;
using namespace lbug::parser;

namespace lbug {
namespace binder {

void validateColumnNamesAreUnique(const std::vector<std::string>& columnNames) {
    auto existColumnNames = std::unordered_set<std::string>();
    for (auto& name : columnNames) {
        if (existColumnNames.contains(name)) {
            throw BinderException(
                "Multiple result columns with the same name " + name + " are not supported.");
        }
        existColumnNames.insert(name);
    }
}

std::vector<std::string> getColumnNames(const expression_vector& exprs,
    const std::vector<std::string>& aliases) {
    std::vector<std::string> columnNames;
    for (auto i = 0u; i < exprs.size(); ++i) {
        if (aliases[i].empty()) {
            columnNames.push_back(exprs[i]->toString());
        } else {
            columnNames.push_back(aliases[i]);
        }
    }
    return columnNames;
}

static void validateOrderByFollowedBySkipOrLimitInWithClause(
    const BoundProjectionBody& boundProjectionBody) {
    auto hasSkipOrLimit = boundProjectionBody.hasSkip() || boundProjectionBody.hasLimit();
    if (boundProjectionBody.hasOrderByExpressions() && !hasSkipOrLimit) {
        throw BinderException("In WITH clause, ORDER BY must be followed by SKIP or LIMIT.");
    }
}

BoundWithClause Binder::bindWithClause(const WithClause& withClause) {
    auto projectionBody = withClause.getProjectionBody();
    auto projectionList = bindProjectionList(*projectionBody);
    auto& [projectionExprs, aliases, _] = projectionList;
    // Check all expressions are aliased
    for (auto& alias : aliases) {
        if (alias.empty()) {
            throw BinderException("Expression in WITH must be aliased (use AS).");
        }
    }
    auto columnNames = getColumnNames(projectionExprs, aliases);
    validateColumnNamesAreUnique(columnNames);
    auto boundProjectionBody = bindProjectionBody(*projectionBody, projectionList);
    validateOrderByFollowedBySkipOrLimitInWithClause(boundProjectionBody);
    // Update scope
    scope.clear();
    for (auto i = 0u; i < projectionExprs.size(); ++i) {
        addToScope(aliases[i], projectionExprs[i]);
    }
    auto boundWithClause = BoundWithClause(std::move(boundProjectionBody));
    if (withClause.hasWhereExpression()) {
        boundWithClause.setWhereExpression(bindWhereExpression(*withClause.getWhereExpression()));
    }
    return boundWithClause;
}

BoundReturnClause Binder::bindReturnClause(const ReturnClause& returnClause) {
    auto projectionBody = returnClause.getProjectionBody();
    auto projectionList = bindProjectionList(*projectionBody);
    auto& [projectionExprs, aliases, _] = projectionList;
    auto columnNames = getColumnNames(projectionExprs, aliases);
    auto boundProjectionBody = bindProjectionBody(*projectionBody, projectionList);
    auto statementResult = BoundStatementResult();
    DASSERT(columnNames.size() == projectionExprs.size());
    for (auto i = 0u; i < columnNames.size(); ++i) {
        statementResult.addColumn(columnNames[i], projectionExprs[i]);
    }
    return BoundReturnClause(std::move(boundProjectionBody), std::move(statementResult));
}

using expr_ptr_set = std::unordered_set<const Expression*>;

// Expressions computed by an earlier clause, compared by identity. Aliases don't identify them:
// RETURN n AS m re-aliases the scope's expression, and a named argument f(a := ...) aliases a
// fresh one. Unique names don't either, as they are structural.
static expr_ptr_set getScopeExprs(const BinderScope& scope) {
    expr_ptr_set result;
    for (auto& expr : scope.getExpressions()) {
        result.insert(expr.get());
    }
    return result;
}

static expression_vector getAggregateExpressions(const std::shared_ptr<Expression>& expression,
    const expr_ptr_set& scopeExprs) {
    expression_vector result;
    if (scopeExprs.contains(expression.get())) {
        return result;
    }
    if (expression->expressionType == ExpressionType::AGGREGATE_FUNCTION) {
        result.push_back(expression);
        return result;
    }
    for (auto& child : ExpressionChildrenCollector::collectChildren(*expression)) {
        for (auto& expr : getAggregateExpressions(child, scopeExprs)) {
            result.push_back(expr);
        }
    }
    return result;
}

BoundProjectionList Binder::bindProjectionList(const ProjectionBody& projectionBody) {
    auto& config = expressionBinder.config;
    auto outerAggregateNames = std::exchange(config.projectionAggregateNames, {});
    auto outerBindingProjectionList = std::exchange(config.bindingProjectionList, true);
    expression_vector projectionExprs;
    std::vector<std::string> aliases;
    std::vector<const ParsedExpression*> parsedItems;
    for (auto& parsedExpr : projectionBody.getProjectionExpressions()) {
        if (parsedExpr->getExpressionType() == ExpressionType::STAR) {
            // Rewrite star expression as all expression in scope.
            if (scope.empty()) {
                throw BinderException(
                    "RETURN or WITH * is not allowed when there are no variables in scope.");
            }
            for (auto& expr : scope.getExpressions()) {
                projectionExprs.push_back(expr);
                aliases.push_back(expr->getAlias());
                parsedItems.push_back(nullptr);
            }
        } else if (parsedExpr->getExpressionType() == ExpressionType::PROPERTY) {
            auto& propExpr = parsedExpr->constCast<ParsedPropertyExpression>();
            if (propExpr.isStar()) {
                // Rewrite property star expression
                for (auto& expr : expressionBinder.bindPropertyStarExpression(*parsedExpr)) {
                    projectionExprs.push_back(expr);
                    aliases.push_back("");
                    parsedItems.push_back(nullptr);
                }
            } else {
                auto expr = expressionBinder.bindExpression(*parsedExpr);
                projectionExprs.push_back(expr);
                aliases.push_back(
                    parsedExpr->hasAlias() ? parsedExpr->getAlias() : expr->getAlias());
                parsedItems.push_back(parsedExpr.get());
            }
        } else {
            auto expr = expressionBinder.bindExpression(*parsedExpr);
            projectionExprs.push_back(expr);
            aliases.push_back(parsedExpr->hasAlias() ? parsedExpr->getAlias() : expr->getAlias());
            parsedItems.push_back(parsedExpr.get());
        }
    }
    config.bindingProjectionList = outerBindingProjectionList;
    config.projectionAggregateNames = std::move(outerAggregateNames);
    return {std::move(projectionExprs), std::move(aliases), std::move(parsedItems)};
}

class NestedAggCollector final : public ExpressionVisitor {
public:
    explicit NestedAggCollector(const expr_ptr_set& scopeExprs) : scopeExprs{scopeExprs} {}

    // Aggregates that are nested inside the visited expression. Aggregates already in scope are
    // not collected: they were computed by an earlier clause.
    expression_vector exprs;

protected:
    void visitAggFunctionExpr(std::shared_ptr<Expression> expr) override {
        if (!scopeExprs.contains(expr.get())) {
            exprs.push_back(std::move(expr));
        }
    }
    void visitChildren(const Expression& expr) override {
        // Do not descend into an expression in scope, e.g. n in
        // WITH COUNT(*) AS c WITH c + 1 AS n RETURN SUM(n).
        if (scopeExprs.contains(&expr)) {
            return;
        }
        switch (expr.expressionType) {
        case ExpressionType::CASE_ELSE: {
            visitCaseExprChildren(expr);
        } break;
        case ExpressionType::LAMBDA: {
            auto& lambda = expr.constCast<LambdaExpression>();
            visit(lambda.getFunctionExpr());
        } break;
        case ExpressionType::AGGREGATE_FUNCTION: {
            // We do not traverse the child or aggregate because nested agg is validated recursively
            // e.g.  WITH SUM(1) AS x WITH SUM(x) AS y RETURN SUM(y)
            // when validating SUM(y) we only need to check y and not x.
        } break;
        default: {
            for (auto& child : expr.getChildren()) {
                visit(child);
            }
        }
        }
    }

private:
    const expr_ptr_set& scopeExprs;
};

static void validateNestedAggregate(const Expression& expr, const expr_ptr_set& scopeExprs) {
    DASSERT(expr.expressionType == ExpressionType::AGGREGATE_FUNCTION);
    if (expr.getNumChildren() == 0) { // Skip COUNT(*)
        return;
    }
    auto collector = NestedAggCollector(scopeExprs);
    collector.visit(expr.getChild(0));
    if (!collector.exprs.empty()) {
        throw BinderException(
            std::format("Expression {} contains nested aggregation.", expr.toString()));
    }
}

namespace {

class VariableNameCollector final : public parser::ParsedExpressionVisitor {
public:
    void visitVariableExpr(const ParsedExpression* expr) override {
        names.insert(expr->constCast<parser::ParsedVariableExpression>().getVariableName());
    }

    std::unordered_set<std::string> names;
};

} // namespace

// After aggregation or DISTINCT, ORDER BY sees only the projected items: under their aliases, or
// their texts when unaliased (items expanded from * or a.* under their names). An aliased item is
// also reachable by its text (RETURN count(*) AS c ORDER BY count(*)), unless the text reads a name
// this projection re-aliases: in RETURN c + 1 AS k, count(*) AS c ORDER BY c + 1, c is count(*).
// Aliases take precedence over texts.
void Binder::addOrderByNamesToScope(const BoundProjectionList& projectionList) {
    auto& [exprs, aliases, parsedItems] = projectionList;
    std::unordered_set<std::string> projectionAliases;
    for (auto i = 0u; i < exprs.size(); ++i) {
        auto* parsed = parsedItems[i];
        if (parsed == nullptr) {
            addToScope(aliases[i].empty() ? exprs[i]->toString() : aliases[i], exprs[i]);
        } else if (parsed->hasAlias()) {
            addToScope(parsed->getAlias(), exprs[i]);
            projectionAliases.insert(parsed->getAlias());
        } else {
            addToScope(parsed->toString(), exprs[i]);
        }
    }
    for (auto i = 0u; i < exprs.size(); ++i) {
        auto* parsed = parsedItems[i];
        if (parsed == nullptr || !parsed->hasAlias() || scope.contains(parsed->toString())) {
            continue;
        }
        VariableNameCollector collector;
        collector.visit(parsed);
        if (std::ranges::any_of(collector.names,
                [&](const std::string& name) { return projectionAliases.contains(name); })) {
            continue;
        }
        addToScope(parsed->toString(), exprs[i]);
    }
}

BoundProjectionBody Binder::bindProjectionBody(const parser::ProjectionBody& projectionBody,
    const BoundProjectionList& projectionList) {
    auto& projectionExprs = projectionList.exprs;
    auto& aliases = projectionList.aliases;
    expression_vector groupByExprs;
    expression_vector aggregateExprs;
    DASSERT(projectionExprs.size() == aliases.size());
    auto scopeExprs = getScopeExprs(scope);
    for (auto i = 0u; i < projectionExprs.size(); ++i) {
        auto expr = projectionExprs[i];
        auto aggExprs = getAggregateExpressions(expr, scopeExprs);
        if (!aggExprs.empty()) {
            for (auto& agg : aggExprs) {
                aggregateExprs.push_back(agg);
            }
        } else {
            groupByExprs.push_back(expr);
        }
        expr->setAlias(aliases[i]);
    }

    auto boundProjectionBody = BoundProjectionBody(projectionBody.getIsDistinct());
    boundProjectionBody.setProjectionExpressions(projectionExprs);

    if (!aggregateExprs.empty()) {
        for (auto& expr : aggregateExprs) {
            validateNestedAggregate(*expr, scopeExprs);
        }
        if (!groupByExprs.empty()) {
            // TODO(Xiyang): we can remove augment group by. But make sure we test sufficient
            // including edge case and bug before release.
            expression_vector augmentedGroupByExpressions = groupByExprs;
            for (auto& expression : groupByExprs) {
                if (ExpressionUtil::isNodePattern(*expression)) {
                    auto& node = expression->constCast<NodeExpression>();
                    augmentedGroupByExpressions.push_back(node.getInternalID());
                } else if (ExpressionUtil::isRelPattern(*expression)) {
                    auto& rel = expression->constCast<RelExpression>();
                    augmentedGroupByExpressions.push_back(rel.getInternalID());
                }
            }
            boundProjectionBody.setGroupByExpressions(std::move(augmentedGroupByExpressions));
        }
        boundProjectionBody.setAggregateExpressions(std::move(aggregateExprs));
    }
    // Bind order by
    if (projectionBody.hasOrderByExpressions()) {
        // Cypher rule of ORDER BY expression scope: if projection contains aggregation, only
        // expressions in projection are available. Otherwise, expressions before projection are
        // also available
        expression_vector orderByExprs;
        if (boundProjectionBody.hasAggregateExpressions() || boundProjectionBody.isDistinct()) {
            scope.clear();
            addOrderByNamesToScope(projectionList);
            expressionBinder.config.bindOrderByAfterAggregate = true;
            orderByExprs = bindOrderByExpressions(projectionBody.getOrderByExpressions());
            expressionBinder.config.bindOrderByAfterAggregate = false;
        } else {
            addToScope(aliases, projectionExprs);
            orderByExprs = bindOrderByExpressions(projectionBody.getOrderByExpressions());
        }
        boundProjectionBody.setOrderByExpressions(std::move(orderByExprs),
            projectionBody.getSortOrders());
    }
    // Bind skip
    if (projectionBody.hasSkipExpression()) {
        boundProjectionBody.setSkipNumber(
            bindSkipLimitExpression(*projectionBody.getSkipExpression()));
    }
    // Bind limit
    if (projectionBody.hasLimitExpression()) {
        boundProjectionBody.setLimitNumber(
            bindSkipLimitExpression(*projectionBody.getLimitExpression()));
    }
    return boundProjectionBody;
}

static bool isOrderByKeyTypeSupported(const LogicalType& dataType) {
    switch (dataType.getLogicalTypeID()) {
    case LogicalTypeID::NODE:
    case LogicalTypeID::REL:
    case LogicalTypeID::RECURSIVE_REL:
    case LogicalTypeID::INTERNAL_ID:
    case LogicalTypeID::LIST:
    case LogicalTypeID::ARRAY:
    case LogicalTypeID::STRUCT:
    case LogicalTypeID::MAP:
    case LogicalTypeID::UNION:
    case LogicalTypeID::POINTER:
        return false;
    default:
        return true;
    }
}

expression_vector Binder::bindOrderByExpressions(
    const std::vector<std::unique_ptr<ParsedExpression>>& parsedExprs) {
    expression_vector exprs;
    for (auto& parsedExpr : parsedExprs) {
        auto expr = expressionBinder.bindExpression(*parsedExpr);
        if (!isOrderByKeyTypeSupported(expr->dataType)) {
            throw BinderException(std::format("Cannot order by {}. Order by {} is not supported.",
                expr->toString(), expr->dataType.toString()));
        }
        exprs.push_back(std::move(expr));
    }
    return exprs;
}

std::shared_ptr<Expression> Binder::bindSkipLimitExpression(const ParsedExpression& expression) {
    auto boundExpression = expressionBinder.bindExpression(expression);
    if (boundExpression->expressionType != ExpressionType::LITERAL &&
        boundExpression->expressionType != ExpressionType::PARAMETER) {
        throw BinderException(
            "The number of rows to skip/limit must be a parameter/literal expression.");
    }
    return boundExpression;
}

} // namespace binder
} // namespace lbug

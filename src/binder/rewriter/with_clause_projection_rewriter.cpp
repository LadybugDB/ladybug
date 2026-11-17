#include "binder/rewriter/with_clause_projection_rewriter.h"

#include "binder/expression/expression_util.h"
#include "binder/expression/node_expression.h"
#include "binder/expression/property_expression.h"
#include "binder/expression/rel_expression.h"
#include "binder/expression_visitor.h"
#include "binder/query/reading_clause/bound_match_clause.h"
#include "binder/visitor/property_collector.h"
#include "common/constants.h"
#include "main/client_context.h"
#include "storage/storage_manager.h"

using namespace lbug::common;

namespace lbug {
namespace binder {

static void rewrite(std::shared_ptr<Expression> expr, expression_vector& projectionList,
    const std::unordered_map<std::string, expression_vector>& varNameToProperties) {
    std::string varName;
    if (ExpressionUtil::isNodePattern(*expr)) {
        auto& node = expr->constCast<NodeExpression>();
        projectionList.push_back(node.getInternalID());
        varName = node.getUniqueName();
    } else if (ExpressionUtil::isRelPattern(*expr)) {
        auto& rel = expr->constCast<RelExpression>();
        projectionList.push_back(rel.getSrcNode()->getInternalID());
        projectionList.push_back(rel.getDstNode()->getInternalID());
        projectionList.push_back(rel.getInternalID());
        if (rel.hasDirectionExpr()) {
            projectionList.push_back(rel.getDirectionExpr());
        }
        varName = rel.getUniqueName();
    } else if (ExpressionUtil::isRecursiveRelPattern(*expr)) {
        auto& rel = expr->constCast<RelExpression>();
        projectionList.push_back(rel.getLengthExpression());
        projectionList.push_back(expr);
        varName = rel.getUniqueName();
    }
    if (!varName.empty()) {
        if (varNameToProperties.contains(varName)) {
            for (auto& property : varNameToProperties.at(varName)) {
                projectionList.push_back(property);
            }
        }
    } else {
        projectionList.push_back(expr);
    }
}

static expression_vector rewrite(const expression_vector& exprs,
    const std::unordered_map<std::string, expression_vector>& varNameToProperties) {
    expression_vector projectionList;
    for (auto& expr : exprs) {
        rewrite(expr, projectionList, varNameToProperties);
    }
    return projectionList;
}

struct MatchedNode {
    std::shared_ptr<NodeExpression> node;
    // A reading clause, or a WITH before the last one, follows the clause that binds the node.
    bool isCarriedFurther = false;
};

// Nodes bound by a regular MATCH before the last query part, by unique name. A node first bound
// by an OPTIONAL MATCH can be NULL, and a fetch by internal ID would drop its row.
static std::unordered_map<std::string, MatchedNode> getMatchedNodes(
    const NormalizedSingleQuery& singleQuery) {
    std::unordered_set<std::string> seen;
    std::unordered_map<std::string, MatchedNode> result;
    auto markCarriedFurther = [&]() {
        for (auto& [name, matched] : result) {
            matched.isCarriedFurther = true;
        }
    };
    for (auto i = 0u; i + 1 < singleQuery.getNumQueryParts(); ++i) {
        // Nodes bound before this part pass through the previous part's WITH.
        markCarriedFurther();
        auto queryPart = singleQuery.getQueryPart(i);
        for (auto j = 0u; j < queryPart->getNumReadingClause(); ++j) {
            markCarriedFurther();
            auto readingClause = queryPart->getReadingClause(j);
            if (readingClause->getClauseType() != ClauseType::MATCH) {
                continue;
            }
            auto& match = readingClause->constCast<BoundMatchClause>();
            for (auto& node : match.getQueryGraphCollection()->getQueryNodes()) {
                if (seen.insert(node->getUniqueName()).second &&
                    match.getMatchClauseType() == MatchClauseType::MATCH) {
                    result.insert({node->getUniqueName(), MatchedNode{node}});
                }
            }
        }
    }
    return result;
}

static bool hasSubquery(const BoundProjectionBody& projectionBody) {
    auto collector = SubqueryExprCollector();
    for (auto& expr : projectionBody.getProjectionExpressions()) {
        collector.visit(expr);
    }
    for (auto& expr : projectionBody.getOrderByExpressions()) {
        collector.visit(expr);
    }
    return collector.hasSubquery();
}

static bool anyEarlierWithOrders(const NormalizedSingleQuery& singleQuery, idx_t numParts) {
    for (auto i = 0u; i < numParts; ++i) {
        if (singleQuery.getQueryPart(i)->getProjectionBody()->hasOrderByExpressions()) {
            return true;
        }
    }
    return false;
}

static bool hasLimitWithinLookupCap(const BoundProjectionBody& projectionBody) {
    if (!projectionBody.hasLimit()) {
        return false;
    }
    auto limit = projectionBody.getLimitNumber();
    return limit->expressionType == ExpressionType::LITERAL &&
           ExpressionUtil::evaluateAsSkipLimit(*limit) <=
               PlannerKnobs::LOOKUP_JOIN_MAX_OUTER_CARDINALITY;
}

// Properties used only in the final RETURN, of nodes passed through a last WITH with an aggregate,
// DISTINCT, SKIP or LIMIT, are fetched by internal ID after that WITH instead of being carried
// through it. That keeps them out of earlier joins, aggregates and sorts, and can leave the node's
// table scan without properties, so RemoveUnnecessaryJoinOptimizer drops it.
// Without a literal LIMIT within LOOKUP_JOIN_MAX_OUTER_CARDINALITY the fetch is a join that reads
// the node table again. That only pays off when a later reading clause or WITH would carry the
// properties too, so a node bound by the last reading clause before the last WITH keeps them.
// Clauses count as written, before NormalizedQueryPartMatchRewriter merges consecutive MATCHes.
static std::vector<LatePropertyFetch> getLatePropertyFetches(const main::ClientContext& context,
    const NormalizedSingleQuery& singleQuery, const expression_set& earlierProperties,
    const expression_vector& lastPartProperties) {
    auto numQueryParts = singleQuery.getNumQueryParts();
    if (numQueryParts < 2 || !context.getClientConfig()->enablePlanOptimizer) {
        return {};
    }
    for (auto i = 0u; i < numQueryParts; ++i) {
        if (singleQuery.getQueryPart(i)->hasUpdatingClause()) {
            return {};
        }
    }
    auto lastPart = singleQuery.getQueryPart(numQueryParts - 1);
    if (lastPart->hasReadingClause() || !lastPart->hasProjectionBody() ||
        hasSubquery(*lastPart->getProjectionBody())) {
        return {};
    }
    auto& barrier = *singleQuery.getQueryPart(numQueryParts - 2)->getProjectionBody();
    if (!barrier.hasAggregateExpressions() && !barrier.isDistinct() && !barrier.hasSkipOrLimit()) {
        return {};
    }
    auto useLookup = hasLimitWithinLookupCap(barrier);
    // After a WITH ORDER BY, the RETURN and any aggregate in it see the rows in that order. A hash
    // join emits them in probe order, so it is used then only if the RETURN sorts without
    // aggregating; rows tied on the RETURN's keys may still come out in another order.
    auto& returnBody = *lastPart->getProjectionBody();
    auto returnSorts = returnBody.hasOrderByExpressions() && !returnBody.hasAggregateExpressions();
    if (!useLookup && !returnSorts && anyEarlierWithOrders(singleQuery, numQueryParts - 1)) {
        return {};
    }
    auto matchedNodes = getMatchedNodes(singleQuery);
    std::unordered_map<std::string, bool> varNameToEligible;
    auto isEligible = [&](const std::string& varName) {
        if (!varNameToEligible.contains(varName)) {
            auto& matched = matchedNodes.at(varName);
            varNameToEligible[varName] = matched.node->getNumEntries() == 1 &&
                                         storage::StorageManager::tryGetNativeNodeTable(context,
                                             *matched.node->getEntry(0)) != nullptr &&
                                         (useLookup || matched.isCarriedFurther);
        }
        return varNameToEligible.at(varName);
    };
    std::vector<LatePropertyFetch> fetches;
    std::unordered_map<std::string, size_t> varNameToFetchIdx;
    for (auto& expr : lastPartProperties) {
        auto& property = expr->constCast<PropertyExpression>();
        if (property.isInternalID() || earlierProperties.contains(expr)) {
            continue;
        }
        auto varName = property.getVariableName();
        if (!matchedNodes.contains(varName) || !isEligible(varName)) {
            continue;
        }
        auto& matched = matchedNodes.at(varName);
        // A node narrowed to one table by a later label keeps the properties of its other tables.
        // Those are NULL, and the scan reads them as such.
        if (!property.hasProperty(matched.node->getEntry(0)->getTableID())) {
            continue;
        }
        if (!varNameToFetchIdx.contains(varName)) {
            varNameToFetchIdx.insert({varName, fetches.size()});
            fetches.push_back(LatePropertyFetch{matched.node, {}, useLookup});
        }
        fetches[varNameToFetchIdx.at(varName)].properties.push_back(expr);
    }
    return fetches;
}

void WithClauseProjectionRewriter::visitSingleQueryUnsafe(NormalizedSingleQuery& singleQuery) {
    DASSERT(singleQuery.getNumQueryParts() != 0);
    auto numQueryParts = singleQuery.getNumQueryParts();
    auto earlierCollector = PropertyCollector();
    for (auto i = 0u; i + 1 < numQueryParts; ++i) {
        earlierCollector.visitQueryPartSkipNodeRel(*singleQuery.getQueryPart(i));
    }
    auto lastPartCollector = PropertyCollector();
    lastPartCollector.visitQueryPart(*singleQuery.getQueryPart(numQueryParts - 1));
    auto earlierProperties = expression_set{};
    for (auto& expr : earlierCollector.getProperties()) {
        earlierProperties.insert(expr);
    }
    auto lastPartProperties = lastPartCollector.getProperties();
    auto lastPart = singleQuery.getQueryPartUnsafe(numQueryParts - 1);
    lastPart->setLatePropertyFetches(
        getLatePropertyFetches(context, singleQuery, earlierProperties, lastPartProperties));
    auto lateProperties = lastPart->getLateProperties();
    auto propertiesToProject = earlierCollector.getProperties();
    for (auto& expr : lastPartProperties) {
        if (!earlierProperties.contains(expr) && !lateProperties.contains(expr)) {
            propertiesToProject.push_back(expr);
        }
    }
    std::unordered_map<std::string, expression_vector> varNameToProperties;
    for (auto& expr : propertiesToProject) {
        auto& property = expr->constCast<PropertyExpression>();
        if (!varNameToProperties.contains(property.getVariableName())) {
            varNameToProperties.insert({property.getVariableName(), expression_vector{}});
        }
        varNameToProperties.at(property.getVariableName()).push_back(expr);
    }
    // Rewrite WITH clause node, relationship pattern projection as node.* & rel.*
    // Because we want to delay the evaluation of node and rel as a struct.
    for (auto i = 0u; i < singleQuery.getNumQueryParts() - 1; ++i) {
        auto queryPart = singleQuery.getQueryPartUnsafe(i);
        auto projectionBody = queryPart->getProjectionBodyUnsafe();
        auto newProjectionExprs =
            rewrite(projectionBody->getProjectionExpressions(), varNameToProperties);
        projectionBody->setProjectionExpressions(std::move(newProjectionExprs));
        auto newGroupByExprs =
            rewrite(projectionBody->getGroupByExpressions(), varNameToProperties);
        projectionBody->setGroupByExpressions(std::move(newGroupByExprs));
    }
    // Remove constant expressions from WITH clause projection list.
    for (auto i = 0u; i < singleQuery.getNumQueryParts() - 1; ++i) {
        auto queryPart = singleQuery.getQueryPartUnsafe(i);
        auto projectionBody = queryPart->getProjectionBodyUnsafe();
        // Avoid rewrite in the case of ORDER BY 1 or aggregate by constant. Because operator
        // implementation replies on expressions to be projected first.
        if (projectionBody->hasOrderByExpressions() || projectionBody->hasAggregateExpressions()) {
            continue;
        }
        expression_vector nonConstantProjectionExprs;
        for (auto& expr : projectionBody->getProjectionExpressions()) {
            if (ConstantExpressionVisitor::isConstant(*expr)) {
                continue;
            }
            nonConstantProjectionExprs.push_back(expr);
        }
        projectionBody->setProjectionExpressions(nonConstantProjectionExprs);
    }
}

} // namespace binder
} // namespace lbug

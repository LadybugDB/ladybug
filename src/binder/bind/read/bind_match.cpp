#include "binder/binder.h"
#include "binder/expression/expression_util.h"
#include "binder/expression/node_expression.h"
#include "binder/query/reading_clause/bound_match_clause.h"
#include "common/exception/binder.h"
#include "function/list/vector_list_functions.h"
#include "main/client_context.h"
#include "main/database_manager.h"
#include "parser/query/reading_clause/match_clause.h"
#include "transaction/transaction.h"
#include <format>

using namespace lbug::common;
using namespace lbug::parser;

namespace lbug {
namespace binder {

static void collectHintPattern(const BoundJoinHintNode& node, binder::expression_set& set) {
    if (node.isLeaf()) {
        set.insert(node.nodeOrRel);
        return;
    }
    for (auto& child : node.children) {
        collectHintPattern(*child, set);
    }
}

static void validateHintCompleteness(const BoundJoinHintNode& root, const QueryGraph& queryGraph) {
    binder::expression_set set;
    collectHintPattern(root, set);
    for (auto& nodeOrRel : queryGraph.getAllPatterns()) {
        if (nodeOrRel->getVariableName().empty()) {
            throw BinderException(
                "Cannot hint join order in a match patter with anonymous node or relationship.");
        }
        if (!set.contains(nodeOrRel)) {
            throw BinderException(
                std::format("Cannot find {} in join hint.", nodeOrRel->toString()));
        }
    }
}

static bool isAnyGraphNodePattern(const NodeOrRelExpression& pattern,
    main::ClientContext* context) {
    if (!ExpressionUtil::isNodePattern(pattern) || pattern.getNumEntries() != 1) {
        return false;
    }
    auto transaction = transaction::Transaction::Get(*context);
    auto useInternal = context->useInternalCatalogEntry();
    auto defaultGraphCatalog = main::DatabaseManager::Get(*context)->getDefaultGraphCatalog();
    if (defaultGraphCatalog == nullptr ||
        !defaultGraphCatalog->containsTable(transaction, "_nodes", useInternal)) {
        return false;
    }
    return pattern.getEntry(0)->getTableID() ==
           defaultGraphCatalog->getTableCatalogEntry(transaction, "_nodes", useInternal)
               ->getTableID();
}

std::unique_ptr<BoundReadingClause> Binder::bindMatchClause(const ReadingClause& readingClause) {
    auto& matchClause = readingClause.constCast<MatchClause>();
    auto boundGraphPattern = bindGraphPattern(matchClause.getPatternElementsRef());
    if (matchClause.hasWherePredicate()) {
        boundGraphPattern.where = bindWhereExpression(*matchClause.getWherePredicate());
    }
    rewriteMatchPattern(boundGraphPattern);
    auto boundMatch = std::make_unique<BoundMatchClause>(
        std::move(boundGraphPattern.queryGraphCollection), matchClause.getMatchClauseType());
    if (matchClause.hasHint()) {
        boundMatch->setHint(
            bindJoinHint(*boundMatch->getQueryGraphCollection(), *matchClause.getHint()));
    }
    boundMatch->setPredicate(boundGraphPattern.where);
    return boundMatch;
}

std::shared_ptr<BoundJoinHintNode> Binder::bindJoinHint(
    const QueryGraphCollection& queryGraphCollection, const JoinHintNode& joinHintNode) {
    if (queryGraphCollection.getNumQueryGraphs() > 1) {
        throw BinderException("Join hint on disconnected match pattern is not supported.");
    }
    auto hint = bindJoinNode(joinHintNode);
    validateHintCompleteness(*hint, *queryGraphCollection.getQueryGraph(0));
    return hint;
}

std::shared_ptr<BoundJoinHintNode> Binder::bindJoinNode(const JoinHintNode& joinHintNode) {
    if (joinHintNode.isLeaf()) {
        std::shared_ptr<Expression> pattern = nullptr;
        if (scope.contains(joinHintNode.variableName)) {
            pattern = scope.getExpression(joinHintNode.variableName);
        }
        if (pattern == nullptr || pattern->expressionType != ExpressionType::PATTERN) {
            throw BinderException(std::format("Cannot bind {} to a node or relationship pattern",
                joinHintNode.variableName));
        }
        return std::make_shared<BoundJoinHintNode>(std::move(pattern));
    }
    auto node = std::make_shared<BoundJoinHintNode>();
    for (auto& child : joinHintNode.children) {
        node->addChild(bindJoinNode(*child));
    }
    return node;
}

void Binder::rewriteMatchPattern(BoundGraphPattern& boundGraphPattern) {
    // Rewrite self loop edge
    // e.g. rewrite (a)-[e]->(a) as [a]-[e]->(b) WHERE id(a) = id(b)
    expression_vector selfLoopEdgePredicates;
    auto& graphCollection = boundGraphPattern.queryGraphCollection;
    for (auto i = 0u; i < graphCollection.getNumQueryGraphs(); ++i) {
        auto queryGraph = graphCollection.getQueryGraphUnsafe(i);
        for (auto& queryRel : queryGraph->getQueryRels()) {
            if (!queryRel->isSelfLoop()) {
                continue;
            }
            auto src = queryRel->getSrcNode();
            auto dst = queryRel->getDstNode();
            auto newDst = createQueryNode(dst->getVariableName(), dst->getEntries());
            queryGraph->addQueryNode(newDst);
            queryRel->setDstNode(newDst);
            auto predicate = expressionBinder.createEqualityComparisonExpression(
                src->getInternalID(), newDst->getInternalID());
            selfLoopEdgePredicates.push_back(std::move(predicate));
        }
    }
    auto where = boundGraphPattern.where;
    for (auto& predicate : selfLoopEdgePredicates) {
        where = expressionBinder.combineBooleanExpressions(ExpressionType::AND, predicate, where);
    }
    // Rewrite key value pairs in MATCH clause as predicate
    for (auto i = 0u; i < graphCollection.getNumQueryGraphs(); ++i) {
        auto queryGraph = graphCollection.getQueryGraphUnsafe(i);
        for (auto& pattern : queryGraph->getAllPatterns()) {
            // In ANY graphs, labels are materialized in _nodes.label as STRING[].
            // A node pattern like (n:A:B) should require all labels to be present.
            if (isAnyGraphNodePattern(*pattern, clientContext)) {
                const auto& labels = pattern->constCast<NodeExpression>().getOriginalLabels();
                if (!labels.empty()) {
                    auto labelExpr =
                        expressionBinder.bindNodeOrRelPropertyExpression(*pattern, "label");
                    for (const auto& label : labels) {
                        auto containsExpr = expressionBinder.bindScalarFunctionExpression(
                            {labelExpr, expressionBinder.createLiteralExpression(label)},
                            function::ListContainsFunction::name);
                        where = expressionBinder.combineBooleanExpressions(ExpressionType::AND,
                            containsExpr, where);
                    }
                }
            }
            for (auto& [propertyName, rhs] : pattern->getPropertyDataExprRef()) {
                auto propertyExpr =
                    expressionBinder.bindNodeOrRelPropertyExpression(*pattern, propertyName);
                auto predicate =
                    expressionBinder.createEqualityComparisonExpression(propertyExpr, rhs);
                where = expressionBinder.combineBooleanExpressions(ExpressionType::AND, predicate,
                    where);
            }
        }
    }
    boundGraphPattern.where = std::move(where);
}

} // namespace binder
} // namespace lbug

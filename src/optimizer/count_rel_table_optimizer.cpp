#include "optimizer/count_rel_table_optimizer.h"

#include "binder/expression/aggregate_function_expression.h"
#include "binder/expression/expression_util.h"
#include "binder/expression/node_expression.h"
#include "binder/expression/property_expression.h"
#include "binder/expression/rel_expression.h"
#include "catalog/catalog_entry/node_table_id_pair.h"
#include "function/aggregate/count.h"
#include "function/aggregate/count_star.h"
#include "main/client_context.h"
#include "planner/operator/extend/logical_extend.h"
#include "planner/operator/logical_aggregate.h"
#include "planner/operator/logical_order_by.h"
#include "planner/operator/logical_projection.h"
#include "planner/operator/scan/logical_count_rel_table.h"
#include "planner/operator/scan/logical_rel_degree_table.h"
#include "planner/operator/scan/logical_scan_node_table.h"

using namespace lbug::common;
using namespace lbug::planner;
using namespace lbug::binder;
using namespace lbug::catalog;

namespace lbug {
namespace optimizer {

void CountRelTableOptimizer::rewrite(LogicalPlan* plan) {
    visitOperator(plan->getLastOperator());
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::visitOperator(
    const std::shared_ptr<LogicalOperator>& op) {
    // bottom-up traversal
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        op->setChild(i, visitOperator(op->getChild(i)));
    }
    auto result = visitOperatorReplaceSwitch(op);
    result->computeFlatSchema();
    return result;
}

bool CountRelTableOptimizer::isSimpleCount(LogicalOperator* op) const {
    if (op->getOperatorType() != LogicalOperatorType::AGGREGATE) {
        return false;
    }
    auto& aggregate = op->constCast<LogicalAggregate>();

    // Must have no keys (i.e., a simple aggregate without GROUP BY)
    if (aggregate.hasKeys()) {
        return false;
    }

    // Must have exactly one aggregate expression
    auto aggregates = aggregate.getAggregates();
    if (aggregates.size() != 1) {
        return false;
    }

    auto& aggExpr = aggregates[0];
    if (aggExpr->expressionType != ExpressionType::AGGREGATE_FUNCTION) {
        return false;
    }
    auto& aggFuncExpr = aggExpr->constCast<AggregateFunctionExpression>();
    const auto& functionName = aggFuncExpr.getFunction().name;
    if (functionName != function::CountStarFunction::name &&
        functionName != function::CountFunction::name) {
        return false;
    }

    if (aggFuncExpr.isDistinct()) {
        return false;
    }

    return true;
}

bool CountRelTableOptimizer::isCountStar(LogicalOperator* op) const {
    auto& aggregate = op->constCast<LogicalAggregate>();
    auto& aggFuncExpr = aggregate.getAggregates()[0]->constCast<AggregateFunctionExpression>();
    return aggFuncExpr.getFunction().name == function::CountStarFunction::name;
}

bool CountRelTableOptimizer::isRelIDExpression(const std::shared_ptr<Expression>& expression,
    const RelExpression& rel) const {
    if (expression->expressionType != ExpressionType::PROPERTY) {
        return false;
    }
    auto& property = expression->constCast<PropertyExpression>();
    return property.isInternalID() && *expression == *rel.getInternalID();
}

bool CountRelTableOptimizer::isCountRelID(LogicalOperator* op, const RelExpression& rel) const {
    auto& aggregate = op->constCast<LogicalAggregate>();
    auto& aggFuncExpr = aggregate.getAggregates()[0]->constCast<AggregateFunctionExpression>();
    if (aggFuncExpr.getFunction().name != function::CountFunction::name) {
        return false;
    }
    if (aggFuncExpr.getNumChildren() != 1) {
        return false;
    }
    return isRelIDExpression(aggFuncExpr.getChild(0), rel);
}

bool CountRelTableOptimizer::isDistinctCountNodeKey(LogicalOperator* op,
    const std::shared_ptr<Expression>& nodeKey) const {
    if (op->getOperatorType() != LogicalOperatorType::AGGREGATE) {
        return false;
    }
    auto& aggregate = op->constCast<LogicalAggregate>();
    if (aggregate.hasKeys() || aggregate.getAggregates().size() != 1) {
        return false;
    }
    auto& aggFuncExpr = aggregate.getAggregates()[0]->constCast<AggregateFunctionExpression>();
    if (aggFuncExpr.getFunction().name != function::CountFunction::name ||
        !aggFuncExpr.isDistinct() || aggFuncExpr.getNumChildren() != 1) {
        return false;
    }
    return *aggFuncExpr.getChild(0) == *nodeKey;
}

bool CountRelTableOptimizer::isCountNbr(LogicalOperator* op, const NodeExpression& nbr) const {
    if (op->getOperatorType() != LogicalOperatorType::AGGREGATE) {
        return false;
    }
    auto& aggregate = op->constCast<LogicalAggregate>();
    if (aggregate.getAggregates().size() != 1) {
        return false;
    }
    auto& aggFuncExpr = aggregate.getAggregates()[0]->constCast<AggregateFunctionExpression>();
    if (aggFuncExpr.isDistinct()) {
        return false;
    }
    if (aggFuncExpr.getFunction().name == function::CountStarFunction::name &&
        aggFuncExpr.getNumChildren() == 0) {
        return true;
    }
    if (aggFuncExpr.getFunction().name != function::CountFunction::name ||
        aggFuncExpr.getNumChildren() != 1) {
        return false;
    }
    return *aggFuncExpr.getChild(0) == *nbr.getInternalID();
}

static bool relTablesForExtend(const LogicalExtend& extend, std::vector<table_id_t>& relTableIDs,
    RelGroupCatalogEntry*& relGroupEntry) {
    auto rel = extend.getRel();
    if (extend.getDirection() != ExtendDirection::FWD || rel->getNumEntries() != 1) {
        return false;
    }
    DASSERT(rel->getNumEntries() == 1);
    relGroupEntry = rel->getEntry(0)->ptrCast<RelGroupCatalogEntry>();
    auto boundNodeTableIDs = extend.getBoundNode()->getTableIDsSet();
    auto nbrNodeTableIDs = extend.getNbrNode()->getTableIDsSet();
    for (auto& info : relGroupEntry->getRelEntryInfos()) {
        bool matches = extend.extendFromSourceNode() ?
                           boundNodeTableIDs.contains(info.nodePair.srcTableID) &&
                               nbrNodeTableIDs.contains(info.nodePair.dstTableID) :
                           boundNodeTableIDs.contains(info.nodePair.dstTableID) &&
                               nbrNodeTableIDs.contains(info.nodePair.srcTableID);
        if (matches) {
            relTableIDs.push_back(info.oid);
        }
    }
    return !relTableIDs.empty();
}

bool CountRelTableOptimizer::canOptimize(LogicalOperator* aggregate) const {
    // Pattern we're looking for:
    // AGGREGATE (COUNT_STAR or COUNT(rel._ID), no keys)
    //   -> PROJECTION (empty expressions, pass-through, or rel._ID)
    //      -> EXTEND (single rel table, no properties scanned)
    //         -> SCAN_NODE_TABLE (no properties scanned)
    //
    // Note: The projection between aggregate and extend might be empty or
    // just projecting the COUNT(rel) input.

    auto* current = aggregate->getChild(0).get();

    std::vector<LogicalProjection*> projections;
    while (current->getOperatorType() == LogicalOperatorType::PROJECTION) {
        projections.push_back(current->ptrCast<LogicalProjection>());
        current = current->getChild(0).get();
    }

    // Now we should have EXTEND
    if (current->getOperatorType() != LogicalOperatorType::EXTEND) {
        return false;
    }
    auto& extend = current->constCast<LogicalExtend>();

    // Don't optimize for undirected edges (BOTH direction) - the query pattern
    // (a)-[e]-(b) generates a plan that scans both directions, and optimizing
    // this would require special handling to avoid double counting.
    if (extend.getDirection() == ExtendDirection::BOTH) {
        return false;
    }

    // The rel should be a single table (not multi-labeled)
    auto rel = extend.getRel();
    if (rel->isMultiLabeled()) {
        return false;
    }

    if (!isCountStar(aggregate) && !isCountRelID(aggregate, *rel)) {
        return false;
    }

    // Check if we're scanning any properties. COUNT(rel) needs only rel._ID; other rel properties
    // would make the relationship variable observable beyond simple cardinality.
    for (auto& property : extend.getProperties()) {
        if (!isRelIDExpression(property, *rel)) {
            return false;
        }
    }

    for (auto* projection : projections) {
        for (auto& expression : projection->getExpressionsToProject()) {
            if (expression->expressionType != ExpressionType::AGGREGATE_FUNCTION &&
                !isRelIDExpression(expression, *rel)) {
                return false;
            }
        }
    }

    // The child of extend should be SCAN_NODE_TABLE
    auto* extendChild = current->getChild(0).get();
    if (extendChild->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE) {
        return false;
    }
    auto& scanNode = extendChild->constCast<LogicalScanNodeTable>();

    // Check if node scan has any properties (we can only optimize when no properties needed)
    if (!scanNode.getProperties().empty()) {
        return false;
    }

    return true;
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::visitAggregateReplace(
    std::shared_ptr<LogicalOperator> op) {
    if (auto rewritten = tryRewriteActiveBoundCount(op); rewritten != op) {
        return rewritten;
    }
    if (!isSimpleCount(op.get())) {
        return op;
    }

    if (!canOptimize(op.get())) {
        return op;
    }

    // Find the EXTEND operator
    auto* current = op->getChild(0).get();
    while (current->getOperatorType() == LogicalOperatorType::PROJECTION) {
        current = current->getChild(0).get();
    }

    DASSERT(current->getOperatorType() == LogicalOperatorType::EXTEND);
    auto& extend = current->constCast<LogicalExtend>();
    auto rel = extend.getRel();
    auto boundNode = extend.getBoundNode();
    auto nbrNode = extend.getNbrNode();

    // Get the rel group entry
    DASSERT(rel->getNumEntries() == 1);
    auto* relGroupEntry = rel->getEntry(0)->ptrCast<RelGroupCatalogEntry>();

    // Determine the source and destination node table IDs based on extend direction.
    // If extendFromSource is true, then boundNode is the source and nbrNode is the destination.
    // If extendFromSource is false, then boundNode is the destination and nbrNode is the source.
    auto boundNodeTableIDs = boundNode->getTableIDsSet();
    auto nbrNodeTableIDs = nbrNode->getTableIDsSet();

    // Get only the rel table IDs that match the specific node table ID pairs in the query.
    // A rel table connects a specific (srcTableID, dstTableID) pair.
    std::vector<table_id_t> relTableIDs;
    for (auto& info : relGroupEntry->getRelEntryInfos()) {
        table_id_t srcTableID = info.nodePair.srcTableID;
        table_id_t dstTableID = info.nodePair.dstTableID;

        bool matches = false;
        if (extend.extendFromSourceNode()) {
            // boundNode is src, nbrNode is dst
            matches =
                boundNodeTableIDs.contains(srcTableID) && nbrNodeTableIDs.contains(dstTableID);
        } else {
            // boundNode is dst, nbrNode is src
            matches =
                boundNodeTableIDs.contains(dstTableID) && nbrNodeTableIDs.contains(srcTableID);
        }

        if (matches) {
            relTableIDs.push_back(info.oid);
        }
    }

    // If no matching rel tables, don't optimize (shouldn't happen for valid queries)
    if (relTableIDs.empty()) {
        return op;
    }

    // Get the count expression from the original aggregate
    auto& aggregate = op->constCast<LogicalAggregate>();
    auto countExpr = aggregate.getAggregates()[0];

    // Get the bound node table IDs as a vector
    std::vector<table_id_t> boundNodeTableIDsVec(boundNodeTableIDs.begin(),
        boundNodeTableIDs.end());

    // Create the new COUNT_REL_TABLE operator with all necessary information for scanning
    auto countRelTable =
        std::make_shared<LogicalCountRelTable>(relGroupEntry, std::move(relTableIDs),
            std::move(boundNodeTableIDsVec), boundNode, extend.getDirection(), countExpr);
    countRelTable->computeFlatSchema();

    return countRelTable;
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::tryRewriteActiveBoundCount(
    std::shared_ptr<LogicalOperator> op) {
    auto* current = op->getChild(0).get();
    while (current->getOperatorType() == LogicalOperatorType::PROJECTION) {
        current = current->getChild(0).get();
    }
    if (current->getOperatorType() != LogicalOperatorType::EXTEND) {
        return op;
    }
    auto& extend = current->constCast<LogicalExtend>();
    auto boundNode = extend.getBoundNode();
    if (boundNode->isMultiLabeled()) {
        return op;
    }
    auto boundKey = boundNode->getPrimaryKey(boundNode->getTableIDs()[0]);
    if (!boundKey || !isDistinctCountNodeKey(op.get(), boundKey)) {
        return op;
    }
    if (!extend.getProperties().empty()) {
        return op;
    }
    auto* scan = current->getChild(0).get();
    if (scan->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE) {
        return op;
    }
    auto& scanNode = scan->constCast<LogicalScanNodeTable>();
    for (auto& property : scanNode.getProperties()) {
        if (!(*property == *boundKey)) {
            return op;
        }
    }
    std::vector<table_id_t> relTableIDs;
    RelGroupCatalogEntry* relGroupEntry = nullptr;
    if (!relTablesForExtend(extend, relTableIDs, relGroupEntry)) {
        return op;
    }
    auto countExpr = op->constCast<LogicalAggregate>().getAggregates()[0];
    auto result =
        std::make_shared<LogicalRelDegreeTable>(relGroupEntry, std::move(relTableIDs), boundNode,
            extend.getDirection(), RelDegreeTableMode::ACTIVE_BOUND_COUNT, boundKey, countExpr, 1);
    result->computeFlatSchema();
    return result;
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::visitOrderByReplace(
    std::shared_ptr<LogicalOperator> op) {
    return tryRewriteDegreeTopK(op);
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::tryRewriteDegreeTopK(
    std::shared_ptr<LogicalOperator> op) {
    auto& orderBy = op->constCast<LogicalOrderBy>();
    if (!orderBy.hasLimitNum() || orderBy.hasSkipNum() ||
        !ExpressionUtil::canEvaluateAsLiteral(*orderBy.getLimitNum()) ||
        orderBy.getExpressionsToOrderBy().size() != 1 || orderBy.getIsAscOrders().size() != 1 ||
        orderBy.getIsAscOrders()[0]) {
        return op;
    }
    const auto limit = ExpressionUtil::evaluateAsSkipLimit(*orderBy.getLimitNum());
    auto* current = op->getChild(0).get();
    while (current->getOperatorType() == LogicalOperatorType::PROJECTION) {
        current = current->getChild(0).get();
    }
    if (current->getOperatorType() != LogicalOperatorType::AGGREGATE) {
        return op;
    }
    auto& aggregate = current->constCast<LogicalAggregate>();
    if (aggregate.getKeys().size() != 1 || aggregate.getAggregates().size() != 1 ||
        aggregate.getDependentKeys().size() != 0 ||
        !(*orderBy.getExpressionsToOrderBy()[0] == *aggregate.getAggregates()[0])) {
        return op;
    }
    auto nodeKey = aggregate.getKeys()[0];
    auto* aggregateChild = current->getChild(0).get();
    while (aggregateChild->getOperatorType() == LogicalOperatorType::PROJECTION) {
        aggregateChild = aggregateChild->getChild(0).get();
    }
    if (aggregateChild->getOperatorType() != LogicalOperatorType::EXTEND) {
        return op;
    }
    auto& extend = aggregateChild->constCast<LogicalExtend>();
    auto boundNode = extend.getBoundNode();
    if (boundNode->isMultiLabeled() ||
        !(*nodeKey == *boundNode->getPrimaryKey(boundNode->getTableIDs()[0])) ||
        !isCountNbr(current, *extend.getNbrNode()) || !extend.getProperties().empty()) {
        return op;
    }
    auto* scan = aggregateChild->getChild(0).get();
    if (scan->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE) {
        return op;
    }
    auto& scanNode = scan->constCast<LogicalScanNodeTable>();
    for (auto& property : scanNode.getProperties()) {
        if (!(*property == *nodeKey)) {
            return op;
        }
    }
    std::vector<table_id_t> relTableIDs;
    RelGroupCatalogEntry* relGroupEntry = nullptr;
    if (!relTablesForExtend(extend, relTableIDs, relGroupEntry)) {
        return op;
    }
    auto result = std::make_shared<LogicalRelDegreeTable>(relGroupEntry, std::move(relTableIDs),
        boundNode, extend.getDirection(), RelDegreeTableMode::TOP_K_DEGREES, nodeKey,
        aggregate.getAggregates()[0], limit);
    result->computeFlatSchema();
    return result;
}

} // namespace optimizer
} // namespace lbug

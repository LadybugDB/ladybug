#include "optimizer/count_rel_table_optimizer.h"

#include "binder/expression/aggregate_function_expression.h"
#include "binder/expression/expression_util.h"
#include "binder/expression/literal_expression.h"
#include "binder/expression/node_expression.h"
#include "binder/expression/property_expression.h"
#include "binder/expression/rel_expression.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/node_table_id_pair.h"
#include "common/enums/path_semantic.h"
#include "function/aggregate/count.h"
#include "function/aggregate/count_star.h"
#include "function/gds/rec_joins.h"
#include "main/client_context.h"
#include "planner/operator/extend/logical_extend.h"
#include "planner/operator/extend/logical_recursive_extend.h"
#include "planner/operator/logical_aggregate.h"
#include "planner/operator/logical_filter.h"
#include "planner/operator/logical_hash_join.h"
#include "planner/operator/logical_order_by.h"
#include "planner/operator/logical_path_property_probe.h"
#include "planner/operator/logical_projection.h"
#include "planner/operator/scan/logical_count_rel_table.h"
#include "planner/operator/scan/logical_reachable_count.h"
#include "planner/operator/scan/logical_rel_degree_table.h"
#include "planner/operator/scan/logical_scan_node_table.h"
#include "storage/storage_manager.h"
#include "storage/table/table.h"

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
    if (auto rewritten = tryRewriteReachableCount(op); rewritten != op) {
        return rewritten;
    }
    if (auto rewritten = tryRewriteActiveBoundCount(op); rewritten != op) {
        return rewritten;
    }
    if (auto rewritten = tryRewriteSortedOffsetCount(op); rewritten != op) {
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
    auto countRelTable = std::make_shared<LogicalCountRelTable>(relGroupEntry,
        std::move(relTableIDs), std::move(boundNodeTableIDsVec), boundNode, extend.getDirection(),
        countExpr, rel->getDbName(relGroupEntry));
    countRelTable->computeFlatSchema();

    return countRelTable;
}

static LogicalOperator* skipProjections(LogicalOperator* op) {
    while (op->getOperatorType() == LogicalOperatorType::PROJECTION) {
        op = op->getChild(0).get();
    }
    return op;
}

static bool isPropertyForNodePrimaryKey(const Expression& expression, const NodeExpression& node) {
    if (expression.expressionType != ExpressionType::PROPERTY || node.getNumEntries() != 1) {
        return false;
    }
    auto& property = expression.constCast<PropertyExpression>();
    return property.getVariableName() == node.getUniqueName() &&
           property.isPrimaryKey(node.getTableIDs()[0]);
}

static bool literalToOffset(const Expression& expression, offset_t& offset) {
    if (expression.expressionType != ExpressionType::LITERAL) {
        return false;
    }
    auto value = expression.constCast<LiteralExpression>().getValue();
    if (value.isNull()) {
        return false;
    }
    switch (value.getDataType().getPhysicalType()) {
    case PhysicalTypeID::INT8: {
        auto signedValue = value.getValue<int8_t>();
        if (signedValue < 0) {
            return false;
        }
        offset = static_cast<offset_t>(signedValue);
        return true;
    }
    case PhysicalTypeID::INT16: {
        auto signedValue = value.getValue<int16_t>();
        if (signedValue < 0) {
            return false;
        }
        offset = static_cast<offset_t>(signedValue);
        return true;
    }
    case PhysicalTypeID::INT32: {
        auto signedValue = value.getValue<int32_t>();
        if (signedValue < 0) {
            return false;
        }
        offset = static_cast<offset_t>(signedValue);
        return true;
    }
    case PhysicalTypeID::INT64: {
        auto signedValue = value.getValue<int64_t>();
        if (signedValue < 0) {
            return false;
        }
        offset = static_cast<offset_t>(signedValue);
        return true;
    }
    case PhysicalTypeID::UINT8: {
        offset = static_cast<offset_t>(value.getValue<uint8_t>());
        return true;
    }
    case PhysicalTypeID::UINT16: {
        offset = static_cast<offset_t>(value.getValue<uint16_t>());
        return true;
    }
    case PhysicalTypeID::UINT32: {
        offset = static_cast<offset_t>(value.getValue<uint32_t>());
        return true;
    }
    case PhysicalTypeID::UINT64: {
        offset = static_cast<offset_t>(value.getValue<uint64_t>());
        return true;
    }
    default:
        return false;
    }
}

static bool getPrimaryKeyOffsetPredicate(const Expression& predicate, const NodeExpression& node,
    offset_t& offset) {
    if (predicate.expressionType != ExpressionType::EQUALS || predicate.getNumChildren() != 2) {
        return false;
    }
    auto lhs = predicate.getChild(0);
    auto rhs = predicate.getChild(1);
    if (isPropertyForNodePrimaryKey(*lhs, node)) {
        return literalToOffset(*rhs, offset);
    }
    if (isPropertyForNodePrimaryKey(*rhs, node)) {
        return literalToOffset(*lhs, offset);
    }
    return false;
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::tryRewriteReachableCount(
    std::shared_ptr<LogicalOperator> op) {
    // Target: AGGREGATE COUNT(DISTINCT <nbr._ID>) with no keys, over a variable-length
    // (a)-[r*lo..up]->(b) path whose source node `a` is fixed to a single node via a primary-key
    // predicate on a CSR-sorted node table. In this case count(distinct b) is exactly the number of
    // distinct nodes reachable from `a` by a walk of any length in [lo, up], which can be computed
    // by a bounded traversal without the recursive extend / hash-join subtree.
    if (op->getOperatorType() != LogicalOperatorType::AGGREGATE) {
        return op;
    }
    auto& aggregate = op->constCast<LogicalAggregate>();
    if (aggregate.hasKeys() || aggregate.getAggregates().size() != 1) {
        return op;
    }
    auto& aggFuncExpr = aggregate.getAggregates()[0]->constCast<AggregateFunctionExpression>();
    if (aggFuncExpr.getFunction().name != function::CountFunction::name ||
        !aggFuncExpr.isDistinct() || aggFuncExpr.getNumChildren() != 1) {
        return op;
    }
    auto countedExpr = aggFuncExpr.getChild(0);

    // Descend through projections to a hash join that binds the recursive extend output with the
    // fixed source node scan.
    auto* current = skipProjections(op->getChild(0).get());
    if (current->getOperatorType() != LogicalOperatorType::HASH_JOIN) {
        return op;
    }

    // Locate the recursive extend within the join subtree.
    LogicalRecursiveExtend* recursiveExtend = nullptr;
    std::function<void(LogicalOperator*)> findRecursive = [&](LogicalOperator* n) {
        if (recursiveExtend != nullptr) {
            return;
        }
        if (n->getOperatorType() == LogicalOperatorType::RECURSIVE_EXTEND) {
            recursiveExtend = n->ptrCast<LogicalRecursiveExtend>();
            return;
        }
        for (auto i = 0u; i < n->getNumChildren(); ++i) {
            findRecursive(n->getChild(i).get());
        }
    };
    findRecursive(current);
    if (recursiveExtend == nullptr) {
        return op;
    }

    auto& bindData = recursiveExtend->getBindData();
    // Only forward variable-length walks are handled. Traversals with a node predicate restrict the
    // reachable set and are left to the (correct) original plan.
    if (bindData.extendDirection != ExtendDirection::FWD ||
        bindData.semantic != common::PathSemantic::WALK || bindData.upperBound == 0 ||
        recursiveExtend->hasNodePredicate()) {
        return op;
    }
    auto boundNode = std::static_pointer_cast<NodeExpression>(bindData.nodeInput);
    auto nbrNode = std::static_pointer_cast<NodeExpression>(bindData.nodeOutput);
    if (boundNode->isMultiLabeled() || nbrNode->isMultiLabeled() ||
        !(*countedExpr == *nbrNode->getInternalID())) {
        return op;
    }

    // Identify the side of the hash join that carries the recursive extend; the other side is the
    // fixed source-node scan of `a`.
    auto subtreeHasRecursive = [](LogicalOperator* n) {
        std::function<bool(LogicalOperator*)> containsRec = [&](LogicalOperator* m) -> bool {
            if (m->getOperatorType() == LogicalOperatorType::RECURSIVE_EXTEND) {
                return true;
            }
            for (auto i = 0u; i < m->getNumChildren(); ++i) {
                if (containsRec(m->getChild(i).get())) {
                    return true;
                }
            }
            return false;
        };
        return containsRec(n);
    };
    auto* leftChild = current->getChild(0).get();
    auto* rightChild = current->getChild(1).get();
    auto* sourceSide = subtreeHasRecursive(leftChild) ? rightChild : leftChild;

    // Navigate to the source scan, skipping projection/semi-masker/filter operators.
    LogicalOperator* source = sourceSide;
    while (source->getOperatorType() == LogicalOperatorType::PROJECTION ||
           source->getOperatorType() == LogicalOperatorType::SEMI_MASKER) {
        source = source->getChild(0).get();
    }
    const LogicalFilter* sourceFilter = nullptr;
    if (source->getOperatorType() == LogicalOperatorType::FILTER) {
        sourceFilter = source->ptrCast<LogicalFilter>();
        source = skipProjections(source->getChild(0).get());
    }
    if (source->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE) {
        return op;
    }
    auto& scan = source->constCast<LogicalScanNodeTable>();

    // Derive the fixed source offset from a primary-key literal. CSR (primary_key == rowid) lets us
    // turn the pk literal directly into a node offset without a lookup.
    offset_t offset = INVALID_OFFSET;
    if (sourceFilter != nullptr) {
        if (!getPrimaryKeyOffsetPredicate(*sourceFilter->getPredicate(), *boundNode, offset)) {
            return op;
        }
    } else if (scan.getScanType() == LogicalScanNodeTableType::PRIMARY_KEY_SCAN &&
               scan.getExtraInfo() != nullptr) {
        auto& primaryKeyScanInfo = scan.getExtraInfo()->constCast<PrimaryKeyScanInfo>();
        if (primaryKeyScanInfo.isRange || !primaryKeyScanInfo.key ||
            !literalToOffset(*primaryKeyScanInfo.key, offset)) {
            return op;
        }
    } else {
        return op;
    }

    // CSR gate: the invariant primary_key == rowid is only an explicit user declaration, so it must
    // be confirmed and unchanged since declaration.
    if (boundNode->getNumEntries() != 1) {
        return op;
    }
    auto tableID = boundNode->getTableIDs()[0];
    auto* nodeEntry = boundNode->getEntry(0)->ptrCast<NodeTableCatalogEntry>();
    if (!nodeEntry->isCsr()) {
        return op;
    }
    auto* table = storage::StorageManager::Get(*_context)->getTable(tableID);
    if (!table || table->getChangeEpoch() != nodeEntry->getCsrChangeEpoch()) {
        return op;
    }

    auto relEntries = bindData.graphEntry.getRelEntries();
    if (relEntries.size() != 1) {
        return op;
    }
    auto* relGroupEntry = relEntries[0]->ptrCast<RelGroupCatalogEntry>();
    auto countExpr = op->constCast<LogicalAggregate>().getAggregates()[0];
    auto result = std::make_shared<LogicalReachableCount>(relGroupEntry, boundNode, nbrNode,
        bindData.extendDirection, bindData.lowerBound, bindData.upperBound, countExpr,
        std::vector<offset_t>{offset});
    result->computeFlatSchema();
    return result;
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::tryRewriteSortedOffsetCount(
    std::shared_ptr<LogicalOperator> op) {
    if (!isSimpleCount(op.get())) {
        return op;
    }
    auto* current = skipProjections(op->getChild(0).get());
    const LogicalFilter* filter = nullptr;
    if (current->getOperatorType() == LogicalOperatorType::FILTER) {
        filter = current->ptrCast<LogicalFilter>();
        current = skipProjections(current->getChild(0).get());
    }
    if (current->getOperatorType() != LogicalOperatorType::EXTEND) {
        return op;
    }
    auto& extend = current->constCast<LogicalExtend>();
    auto boundNode = extend.getBoundNode();
    if (boundNode->getNumEntries() != 1 || extend.getDirection() == ExtendDirection::BOTH ||
        !extend.getProperties().empty()) {
        return op;
    }
    auto tableID = boundNode->getTableIDs()[0];
    auto* nodeEntry = boundNode->getEntry(0)->ptrCast<NodeTableCatalogEntry>();
    // The CSR declaration asserts primary_key == rowid (csr_index interchangeable with the rel
    // table's table_offset). This is only an explicit user declaration, not a derivable fact from
    // the sort order, so it must be gated on the CSR flag.
    if (!nodeEntry->isCsr()) {
        return op;
    }
    // Any mutation of the node table invalidates the CSR invariant, so disregard the
    // optimization if the table has been mutated since the CSR declaration.
    auto* table = storage::StorageManager::Get(*_context)->getTable(tableID);
    if (!table || table->getChangeEpoch() != nodeEntry->getCsrChangeEpoch()) {
        return op;
    }
    auto nodeKey = boundNode->getPrimaryKey(tableID);
    if (!nodeKey) {
        return op;
    }
    auto* scan = current->getChild(0).get();
    if (scan->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE) {
        return op;
    }
    auto& scanNode = scan->constCast<LogicalScanNodeTable>();
    offset_t offset = INVALID_OFFSET;
    if (filter) {
        if (!getPrimaryKeyOffsetPredicate(*filter->getPredicate(), *boundNode, offset)) {
            return op;
        }
    } else {
        if (scanNode.getScanType() != LogicalScanNodeTableType::PRIMARY_KEY_SCAN ||
            scanNode.getExtraInfo() == nullptr) {
            return op;
        }
        auto& primaryKeyScanInfo = scanNode.getExtraInfo()->constCast<PrimaryKeyScanInfo>();
        if (primaryKeyScanInfo.isRange || !primaryKeyScanInfo.key ||
            !literalToOffset(*primaryKeyScanInfo.key, offset)) {
            return op;
        }
    }
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
    auto countExpr = op->constCast<LogicalAggregate>().getAggregates()[0];
    auto result =
        std::make_shared<LogicalRelDegreeTable>(relGroupEntry, std::move(relTableIDs), boundNode,
            extend.getDirection(), RelDegreeTableMode::OFFSET_COUNT, nodeKey, countExpr, 1, offset);
    result->computeFlatSchema();
    return result;
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

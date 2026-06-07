#include "planner/join_order/cardinality_estimator.h"

#include "binder/expression/property_expression.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/enums/extend_direction_util.h"
#include "main/client_context.h"
#include "planner/join_order/join_order_util.h"
#include "planner/operator/logical_aggregate.h"
#include "planner/operator/logical_hash_join.h"
#include "planner/operator/scan/logical_scan_node_table.h"
#include "storage/stats/planner_stats.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::transaction;

namespace lbug {
namespace planner {

static cardinality_t atLeastOne(uint64_t x) {
    return x == 0 ? 1 : x;
}

static PlannerTableStats getPlannerStats(main::ClientContext* context,
    storage::StorageManager& storageManager, catalog::TableCatalogEntry& tableEntry,
    table_id_t physicalTableID = INVALID_TABLE_ID) {
    const auto tableID =
        physicalTableID == INVALID_TABLE_ID ? tableEntry.getTableID() : physicalTableID;
    auto* table = storageManager.getTable(tableID);
    if (auto cachedStats = storageManager.getCachedPlannerTableStats(tableID);
        cachedStats.has_value() && cachedStats->tableChangeEpoch == table->getChangeEpoch()) {
        return std::move(cachedStats.value());
    }
    return storage::buildPlannerTableStats(storageManager, *catalog::Catalog::Get(*context),
        transaction::Transaction::Get(*context), tableEntry, physicalTableID);
}

void CardinalityEstimator::init(const QueryGraph& queryGraph) {
    for (auto i = 0u; i < queryGraph.getNumQueryNodes(); ++i) {
        init(*queryGraph.getQueryNode(i));
    }
    for (uint64_t i = 0u; i < queryGraph.getNumQueryRels(); ++i) {
        auto rel = queryGraph.getQueryRel(i);
        init(*rel);
        if (QueryRelTypeUtils::isRecursive(rel->getRelType())) {
            auto recursiveInfo = rel->getRecursiveInfo();
            init(*recursiveInfo->node);
            init(*recursiveInfo->rel);
        }
    }
}

void CardinalityEstimator::init(const NodeExpression& node) {
    auto key = node.getInternalID()->getUniqueName();
    cardinality_t numNodes = 0u;
    auto storageManager = storage::StorageManager::Get(*context);
    for (auto entry : node.getEntries()) {
        // Skip foreign tables - they don't have storage in the local database
        if (entry->getType() == catalog::CatalogEntryType::FOREIGN_TABLE_ENTRY) {
            continue;
        }
        auto tableID = entry->getTableID();
        auto stats = getPlannerStats(context, *storageManager, *entry);
        DASSERT(stats.storageStats.has_value());
        numNodes += stats.storageStats->getTableCard();
        if (!tableStats.contains(tableID)) {
            tableStats.insert({tableID, std::move(stats)});
        }
    }
    if (!nodeIDName2dom.contains(key)) {
        nodeIDName2dom.insert({key, numNodes});
    }
}

void CardinalityEstimator::init(const RelExpression& rel) {
    auto storageManager = storage::StorageManager::Get(*context);
    for (auto entry : rel.getEntries()) {
        if (entry->getType() == catalog::CatalogEntryType::FOREIGN_TABLE_ENTRY) {
            continue;
        }
        auto& relGroupEntry = entry->cast<catalog::RelGroupCatalogEntry>();
        for (const auto& relInfo : relGroupEntry.getRelEntryInfos()) {
            const auto tableID = relInfo.oid;
            if (tableStats.contains(tableID)) {
                continue;
            }
            tableStats.insert(
                {tableID, getPlannerStats(context, *storageManager, *entry, tableID)});
        }
    }
}

void CardinalityEstimator::rectifyCardinality(const Expression& nodeID, cardinality_t card) {
    DASSERT(nodeIDName2dom.contains(nodeID.getUniqueName()));
    auto newCard = std::min(nodeIDName2dom.at(nodeID.getUniqueName()), card);
    nodeIDName2dom[nodeID.getUniqueName()] = newCard;
}

cardinality_t CardinalityEstimator::getNodeIDDom(const std::string& nodeIDName) const {
    DASSERT(nodeIDName2dom.contains(nodeIDName));
    return nodeIDName2dom.at(nodeIDName);
}

uint64_t CardinalityEstimator::estimateScanNode(const LogicalOperator& op) const {
    const auto& scan = op.constCast<const LogicalScanNodeTable&>();
    switch (scan.getScanType()) {
    case LogicalScanNodeTableType::PRIMARY_KEY_SCAN: {
        auto& primaryKeyScanInfo = scan.getExtraInfo()->constCast<PrimaryKeyScanInfo>();
        return primaryKeyScanInfo.isRange ?
                   atLeastOne(getNodeIDDom(scan.getNodeID()->getUniqueName())) :
                   1;
    }
    default:
        return atLeastOne(getNodeIDDom(scan.getNodeID()->getUniqueName()));
    }
}

uint64_t CardinalityEstimator::estimateAggregate(const LogicalAggregate& op) const {
    // TODO(Royi) we can use HLL to better estimate the number of distinct keys here
    return op.getKeys().empty() ? 1 : op.getChild(0)->getCardinality();
}

cardinality_t CardinalityEstimator::multiply(double extensionRate, cardinality_t card) const {
    return atLeastOne(extensionRate * card);
}

uint64_t CardinalityEstimator::estimateHashJoin(
    const std::vector<binder::expression_pair>& joinConditions, const LogicalOperator& probeOp,
    const LogicalOperator& buildOp) const {
    if (LogicalHashJoin::isNodeIDOnlyJoin(joinConditions)) {
        cardinality_t denominator = 1u;
        auto joinKeys = LogicalHashJoin::getJoinNodeIDs(joinConditions);
        for (auto& joinKey : joinKeys) {
            if (nodeIDName2dom.contains(joinKey->getUniqueName())) {
                denominator *= getNodeIDDom(joinKey->getUniqueName());
            }
        }
        return atLeastOne(probeOp.getCardinality() *
                          JoinOrderUtil::getJoinKeysFlatCardinality(joinKeys, buildOp) /
                          atLeastOne(denominator));
    } else {
        // Naively estimate the cardinality if the join is non-ID based
        cardinality_t estCardinality = probeOp.getCardinality() * buildOp.getCardinality();
        for (size_t i = 0; i < joinConditions.size(); ++i) {
            estCardinality *= PlannerKnobs::EQUALITY_PREDICATE_SELECTIVITY;
        }
        return atLeastOne(estCardinality);
    }
}

uint64_t CardinalityEstimator::estimateCrossProduct(const LogicalOperator& probeOp,
    const LogicalOperator& buildOp) const {
    return atLeastOne(probeOp.getCardinality() * buildOp.getCardinality());
}

uint64_t CardinalityEstimator::estimateIntersect(const expression_vector& joinNodeIDs,
    const LogicalOperator& probeOp, const std::vector<LogicalOperator*>& buildOps) const {
    // Formula 1: treat intersect as a Filter on probe side.
    uint64_t estCardinality1 =
        probeOp.getCardinality() * PlannerKnobs::NON_EQUALITY_PREDICATE_SELECTIVITY;
    // Formula 2: assume independence on join conditions.
    cardinality_t denominator = 1u;
    for (auto& joinNodeID : joinNodeIDs) {
        denominator *= getNodeIDDom(joinNodeID->getUniqueName());
    }
    auto numerator = probeOp.getCardinality();
    for (auto& buildOp : buildOps) {
        numerator *= buildOp->getCardinality();
    }
    auto estCardinality2 = numerator / atLeastOne(denominator);
    // Pick minimum between the two formulas.
    return atLeastOne(std::min<uint64_t>(estCardinality1, estCardinality2));
}

uint64_t CardinalityEstimator::estimateFlatten(const LogicalOperator& childOp,
    f_group_pos groupPosToFlatten) const {
    auto group = childOp.getSchema()->getGroup(groupPosToFlatten);
    return atLeastOne(childOp.getCardinality() * group->cardinalityMultiplier);
}

static bool isPrimaryKey(const Expression& expression) {
    if (expression.expressionType != ExpressionType::PROPERTY) {
        return false;
    }
    return ((PropertyExpression&)expression).isPrimaryKey();
}

static bool isSingleLabelledProperty(const Expression& expression) {
    if (expression.expressionType != ExpressionType::PROPERTY) {
        return false;
    }
    return expression.constCast<PropertyExpression>().isSingleLabel();
}

static std::optional<cardinality_t> getTableStatsIfPossible(main::ClientContext* context,
    const Expression& predicate,
    const std::unordered_map<common::table_id_t, PlannerTableStats>& tableStats) {
    DASSERT(predicate.getNumChildren() >= 1);
    if (isSingleLabelledProperty(*predicate.getChild(0))) {
        auto& propertyExpr = predicate.getChild(0)->cast<PropertyExpression>();
        auto tableID = propertyExpr.getSingleTableID();
        if (tableStats.contains(tableID) && tableStats.at(tableID).storageStats.has_value() &&
            propertyExpr.hasProperty(tableID)) {
            auto transaction = Transaction::Get(*context);
            auto entry =
                catalog::Catalog::Get(*context)->getTableCatalogEntry(transaction, tableID);
            if (!entry->containsProperty(propertyExpr.getPropertyName())) {
                return {};
            }
            auto columnID = entry->getColumnID(propertyExpr.getPropertyName());
            if (columnID != INVALID_COLUMN_ID && columnID != ROW_IDX_COLUMN_ID) {
                auto& stats = tableStats.at(tableID).storageStats.value();
                return atLeastOne(stats.getNumDistinctValues(columnID));
            }
        }
    }
    return {};
}

uint64_t CardinalityEstimator::estimateFilter(const LogicalOperator& childPlan,
    const Expression& predicate) const {
    if (predicate.expressionType == ExpressionType::EQUALS) {
        if (isPrimaryKey(*predicate.getChild(0)) || isPrimaryKey(*predicate.getChild(1))) {
            return 1;
        } else {
            const auto numDistinctValues = getTableStatsIfPossible(context, predicate, tableStats);
            if (numDistinctValues.has_value()) {
                return atLeastOne(childPlan.getCardinality() / numDistinctValues.value());
            }
            return atLeastOne(
                childPlan.getCardinality() * PlannerKnobs::EQUALITY_PREDICATE_SELECTIVITY);
        }
    } else {
        return atLeastOne(
            childPlan.getCardinality() * PlannerKnobs::NON_EQUALITY_PREDICATE_SELECTIVITY);
    }
}

uint64_t CardinalityEstimator::getNumNodes(const Transaction*,
    const std::vector<table_id_t>& tableIDs) const {
    cardinality_t numNodes = 0u;
    for (auto& tableID : tableIDs) {
        // Skip foreign tables - they won't be in tableStats.
        if (!tableStats.contains(tableID) || !tableStats.at(tableID).storageStats.has_value()) {
            continue;
        }
        numNodes += tableStats.at(tableID).storageStats.value().getTableCard();
    }
    return atLeastOne(numNodes);
}

uint64_t CardinalityEstimator::getNumRels(const Transaction* transaction,
    const std::vector<table_id_t>& tableIDs) const {
    cardinality_t numRels = 0u;
    for (auto tableID : tableIDs) {
        numRels +=
            storage::StorageManager::Get(*context)->getTable(tableID)->getNumTotalRows(transaction);
    }
    return atLeastOne(numRels);
}

double CardinalityEstimator::getOneHopExtensionRate(const std::vector<table_id_t>& tableIDs,
    const std::vector<table_id_t>& boundTableIDs, RelDataDirection direction) const {
    cardinality_t numBoundNodes = 0;
    cardinality_t numRels = 0;
    for (auto tableID : boundTableIDs) {
        if (tableStats.contains(tableID) && tableStats.at(tableID).storageStats.has_value()) {
            numBoundNodes += tableStats.at(tableID).storageStats.value().getTableCard();
        }
    }
    for (auto tableID : tableIDs) {
        if (!tableStats.contains(tableID)) {
            continue;
        }
        const auto& stats = tableStats.at(tableID);
        const auto directionKey = RelDirectionUtils::relDirectionToKeyIdx(direction);
        if (!stats.relDirectionStats[directionKey].has_value()) {
            continue;
        }
        const auto& relStats = stats.relDirectionStats[directionKey].value();
        numRels += relStats.numRows;
    }
    return static_cast<double>(numRels) / atLeastOne(numBoundNodes);
}

double CardinalityEstimator::getExtensionRate(const RelExpression& rel,
    const NodeExpression& boundNode, ExtendDirection direction,
    const Transaction* transaction) const {
    double oneHopExtensionRate = 0;
    switch (direction) {
    case ExtendDirection::FWD:
    case ExtendDirection::BWD: {
        oneHopExtensionRate = getOneHopExtensionRate(rel.getInnerRelTableIDs(),
            boundNode.getTableIDs(), ExtendDirectionUtil::getRelDataDirection(direction));
    } break;
    case ExtendDirection::BOTH: {
        oneHopExtensionRate = getOneHopExtensionRate(rel.getInnerRelTableIDs(),
                                  boundNode.getTableIDs(), RelDataDirection::FWD) +
                              getOneHopExtensionRate(rel.getInnerRelTableIDs(),
                                  boundNode.getTableIDs(), RelDataDirection::BWD);
    } break;
    default:
        UNREACHABLE_CODE;
    }
    const auto numRels = static_cast<double>(getNumRels(transaction, rel.getInnerRelTableIDs()));
    switch (rel.getRelType()) {
    case QueryRelType::NON_RECURSIVE: {
        return oneHopExtensionRate;
    }
    case QueryRelType::VARIABLE_LENGTH_WALK:
    case QueryRelType::VARIABLE_LENGTH_TRAIL:
    case QueryRelType::VARIABLE_LENGTH_ACYCLIC: {
        auto rate = oneHopExtensionRate *
                    std::max<uint16_t>(rel.getRecursiveInfo()->bindData->upperBound, 1);
        rate = std::min(rate, numRels);
        return rate * context->getClientConfig()->recursivePatternCardinalityScaleFactor;
    }
    case QueryRelType::SHORTEST:
    case QueryRelType::ALL_SHORTEST:
    case QueryRelType::WEIGHTED_SHORTEST:
    case QueryRelType::ALL_WEIGHTED_SHORTEST: {
        auto rate = std::min<double>(
            oneHopExtensionRate *
                std::max<uint16_t>(rel.getRecursiveInfo()->bindData->upperBound, 1),
            numRels);
        return rate * context->getClientConfig()->recursivePatternCardinalityScaleFactor;
    }
    default:
        UNREACHABLE_CODE;
    }
}

} // namespace planner
} // namespace lbug

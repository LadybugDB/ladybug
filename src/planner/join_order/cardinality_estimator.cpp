#include "planner/join_order/cardinality_estimator.h"

#include "binder/expression/literal_expression.h"
#include "binder/expression/property_expression.h"
#include "binder/expression/scalar_function_expression.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/enums/extend_direction_util.h"
#include "function/list/vector_list_functions.h"
#include "main/client_context.h"
#include "main/database_manager.h"
#include "planner/join_order/join_order_util.h"
#include "planner/operator/logical_aggregate.h"
#include "planner/operator/logical_hash_join.h"
#include "planner/operator/scan/logical_scan_node_table.h"
#include "storage/partition_storage_registry.h"
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
    storage::StorageManager& storageManager, catalog::Catalog& catalog,
    catalog::TableCatalogEntry& tableEntry, table_id_t physicalTableID = INVALID_TABLE_ID) {
    const auto tableID =
        physicalTableID == INVALID_TABLE_ID ? tableEntry.getTableID() : physicalTableID;
    auto* table = storageManager.containsTable(tableID) ?
                      storageManager.getTable(tableID) :
                      // Partition children resolve through their own StorageManager.
                      storage::PartitionStorageRegistry::resolveNodeTableByID(context, tableID);
    if (auto cachedStats = storageManager.getCachedPlannerTableStats(tableID);
        cachedStats.has_value() && cachedStats->tableChangeEpoch == table->getChangeEpoch()) {
        return std::move(cachedStats.value());
    }
    // No fresh cache entry: build full ANALYZE stats (real CSR degree distributions for
    // rel tables) so one-hop extend fan-out estimation sees true degrees instead of
    // schema-only row counts, and cache them for subsequent plannings on this epoch.
    auto stats = storage::buildPlannerTableStats(storageManager, catalog,
        transaction::Transaction::Get(*context), tableEntry, physicalTableID,
        storage::PlannerStatsMode::ANALYZE);
    storageManager.setCachedPlannerTableStats(stats.copy());
    return stats;
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
    for (auto entry : node.getEntries()) {
        // Skip foreign tables and other scan-function-backed entries (e.g. remotely routed
        // partition substitutes) - they don't have storage in the local database.
        if (entry->getType() == catalog::CatalogEntryType::FOREIGN_TABLE_ENTRY ||
            (entry->getType() == catalog::CatalogEntryType::NODE_TABLE_ENTRY &&
                entry->ptrCast<catalog::NodeTableCatalogEntry>()->getScanFunction().has_value())) {
            continue;
        }
        auto tableID = entry->getTableID();
        auto dbName = node.getDbName(entry);
        if (!dbName.empty()) {
            // Entries from non-lbug attached databases (DuckDB, Postgres,
            // Iceberg, ...) have no local storage stats. Skipping them also
            // avoids downcasting a foreign attached database to
            // AttachedLbugDatabase, which is undefined behaviour.
            auto* attachedDB = main::DatabaseManager::Get(*context)->getAttachedDatabase(dbName);
            if (attachedDB == nullptr || attachedDB->getDBType() != common::ATTACHED_LBUG_DB_TYPE) {
                continue;
            }
        }
        storage::StorageManager* storageManager;
        catalog::Catalog* cat;
        if (!dbName.empty()) {
            auto* attachedDB = main::DatabaseManager::Get(*context)->getAttachedDatabase(dbName);
            auto* attachedLbugDB = static_cast<main::AttachedLbugDatabase*>(attachedDB);
            storageManager = attachedLbugDB->getStorageManager();
            cat = attachedDB->getCatalog();
        } else {
            storageManager = storage::StorageManager::Get(*context);
            cat = catalog::Catalog::Get(*context);
        }
        auto stats = getPlannerStats(context, *storageManager, *cat, *entry);
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
    for (auto entry : rel.getEntries()) {
        if (entry->getType() == catalog::CatalogEntryType::FOREIGN_TABLE_ENTRY) {
            continue;
        }
        auto dbName = rel.getDbName(entry);
        if (!dbName.empty()) {
            // See the node overload above: foreign attached tables carry no
            // local stats and must not be downcast to AttachedLbugDatabase.
            auto* attachedDB = main::DatabaseManager::Get(*context)->getAttachedDatabase(dbName);
            if (attachedDB == nullptr || attachedDB->getDBType() != common::ATTACHED_LBUG_DB_TYPE) {
                continue;
            }
        }
        storage::StorageManager* storageManager;
        catalog::Catalog* cat;
        if (!dbName.empty()) {
            auto* attachedDB = main::DatabaseManager::Get(*context)->getAttachedDatabase(dbName);
            auto* attachedLbugDB = static_cast<main::AttachedLbugDatabase*>(attachedDB);
            storageManager = attachedLbugDB->getStorageManager();
            cat = attachedDB->getCatalog();
        } else {
            storageManager = storage::StorageManager::Get(*context);
            cat = catalog::Catalog::Get(*context);
        }
        auto& relGroupEntry = entry->cast<catalog::RelGroupCatalogEntry>();
        for (const auto& relInfo : relGroupEntry.getRelEntryInfos()) {
            const auto tableID = relInfo.oid;
            if (tableStats.contains(tableID)) {
                continue;
            }
            tableStats.insert(
                {tableID, getPlannerStats(context, *storageManager, *cat, *entry, tableID)});
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
    case LogicalScanNodeTableType::SECONDARY_INDEX_SCAN:
        return atLeastOne(getNodeIDDom(scan.getNodeID()->getUniqueName()));
    default:
        return atLeastOne(getNodeIDDom(scan.getNodeID()->getUniqueName()));
    }
}

uint64_t CardinalityEstimator::estimateAggregate(const LogicalAggregate& op) const {
    if (op.getKeys().empty()) {
        return 1;
    }
    // Output rows = number of distinct key combinations, bounded above by both the input
    // rows and the product of per-key domains. Any key with an unknown domain falls back
    // to the input cardinality (previous behavior).
    const auto childCard = static_cast<double>(op.getChild(0)->getCardinality());
    auto domainProduct = 1.0;
    for (auto& key : op.getKeys()) {
        auto domain = getGroupKeyDomain(*key);
        if (!domain.has_value()) {
            return op.getChild(0)->getCardinality();
        }
        domainProduct *= static_cast<double>(domain.value());
        if (domainProduct >= childCard) {
            return op.getChild(0)->getCardinality();
        }
    }
    return atLeastOne(static_cast<cardinality_t>(std::min(domainProduct, childCard)));
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

uint64_t CardinalityEstimator::estimateHashJoin(const expression_vector& joinNodeIDs,
    const LogicalOperator& probeOp, const LogicalOperator& buildOp) const {
    std::vector<expression_pair> joinConditions;
    joinConditions.reserve(joinNodeIDs.size());
    for (auto& joinNodeID : joinNodeIDs) {
        joinConditions.emplace_back(joinNodeID, joinNodeID);
    }
    return estimateHashJoin(joinConditions, probeOp, buildOp);
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

static std::optional<cardinality_t> getPropertyNumDistinct(main::ClientContext* context,
    const Expression& propertyExpr,
    const std::unordered_map<common::table_id_t, PlannerTableStats>& tableStats) {
    if (!isSingleLabelledProperty(propertyExpr)) {
        return {};
    }
    auto& prop = propertyExpr.constCast<PropertyExpression>();
    auto tableID = prop.getSingleTableID();
    if (tableStats.contains(tableID) && tableStats.at(tableID).storageStats.has_value() &&
        prop.hasProperty(tableID)) {
        auto transaction = Transaction::Get(*context);
        auto [cat, sm] = main::DatabaseManager::resolveTableStorage(*context, tableID);
        auto entry = cat->getTableCatalogEntry(transaction, tableID);
        if (!entry->containsProperty(prop.getPropertyName())) {
            return {};
        }
        auto columnID = entry->getColumnID(prop.getPropertyName());
        if (columnID != INVALID_COLUMN_ID && columnID != ROW_IDX_COLUMN_ID) {
            auto& stats = tableStats.at(tableID).storageStats.value();
            return atLeastOne(stats.getNumDistinctValues(columnID));
        }
    }
    return {};
}

static std::optional<cardinality_t> getTableStatsIfPossible(main::ClientContext* context,
    const Expression& predicate,
    const std::unordered_map<common::table_id_t, PlannerTableStats>& tableStats) {
    DASSERT(predicate.getNumChildren() >= 1);
    return getPropertyNumDistinct(context, *predicate.getChild(0), tableStats);
}

// Observed [min, max] of a single-label property's column, for range-selectivity
// estimation. Empty when stats are unavailable or the type is not min/max-tracked.
static std::optional<std::pair<double, double>> getPropertyMinMax(main::ClientContext* context,
    const Expression& propertyExpr,
    const std::unordered_map<common::table_id_t, PlannerTableStats>& tableStats) {
    if (!isSingleLabelledProperty(propertyExpr)) {
        return {};
    }
    auto& prop = propertyExpr.constCast<PropertyExpression>();
    auto tableID = prop.getSingleTableID();
    if (!tableStats.contains(tableID) || !tableStats.at(tableID).storageStats.has_value() ||
        !prop.hasProperty(tableID)) {
        return {};
    }
    auto transaction = Transaction::Get(*context);
    auto [cat, sm] = main::DatabaseManager::resolveTableStorage(*context, tableID);
    auto entry = cat->getTableCatalogEntry(transaction, tableID);
    if (!entry->containsProperty(prop.getPropertyName())) {
        return {};
    }
    auto columnID = entry->getColumnID(prop.getPropertyName());
    if (columnID == INVALID_COLUMN_ID || columnID == ROW_IDX_COLUMN_ID) {
        return {};
    }
    return tableStats.at(tableID).storageStats.value().getColumnMinMax(columnID);
}

// Converts a folded literal to double in the column's physical domain (integers, DATE
// days, TIMESTAMP micros), matching ColumnStats::updateMinMax. Parameters and other
// non-constant expressions stay on the fixed-selectivity path: their values are
// unknown at plan time.
static std::optional<double> literalToDouble(const Expression& expr) {
    if (expr.expressionType != ExpressionType::LITERAL) {
        return {};
    }
    const auto& value = expr.constCast<LiteralExpression>().getValue();
    if (value.isNull()) {
        return {};
    }
    switch (value.getDataType().getLogicalTypeID()) {
    case LogicalTypeID::INT8:
        return value.getValue<int8_t>();
    case LogicalTypeID::INT16:
        return value.getValue<int16_t>();
    case LogicalTypeID::INT32:
        return value.getValue<int32_t>();
    case LogicalTypeID::INT64:
        return static_cast<double>(value.getValue<int64_t>());
    case LogicalTypeID::UINT8:
        return value.getValue<uint8_t>();
    case LogicalTypeID::UINT16:
        return value.getValue<uint16_t>();
    case LogicalTypeID::UINT32:
        return value.getValue<uint32_t>();
    case LogicalTypeID::UINT64:
        return static_cast<double>(value.getValue<uint64_t>());
    case LogicalTypeID::FLOAT:
        return value.getValue<float>();
    case LogicalTypeID::DOUBLE:
        return value.getValue<double>();
    case LogicalTypeID::DATE:
        return value.getValue<date_t>().days;
    case LogicalTypeID::TIMESTAMP:
    case LogicalTypeID::TIMESTAMP_TZ:
    case LogicalTypeID::TIMESTAMP_MS:
    case LogicalTypeID::TIMESTAMP_NS:
    case LogicalTypeID::TIMESTAMP_SEC:
        return static_cast<double>(value.getValue<timestamp_t>().value);
    default:
        return {};
    }
}

// Strips binder-inserted CASTs (e.g. an INT32 column vs an INT64 literal compares as
// CAST(prop) OP literal) to the underlying property. Value-preserving numeric and
// temporal casts keep the range fraction valid.
static const Expression* unwrapCast(const Expression* expr) {
    while (expr->expressionType == ExpressionType::FUNCTION && expr->getNumChildren() == 1) {
        const auto& name = expr->constCast<ScalarFunctionExpression>().getFunction().name;
        if (std::string(name).rfind("CAST", 0) != 0) {
            break;
        }
        expr = expr->getChild(0).get();
    }
    return expr;
}

// Selectivity of `property OP constant` for ordered comparisons, as the fraction of the
// observed [min, max] range satisfying OP under uniformity, clamped to [0, 1]. A filter
// keeping every row (e.g. `c.length >= 0` when min is 0) estimates 1.0 instead of the
// fixed 0.1 below, so the DP comparison sees true cardinalities; a filter outside the
// range estimates 0 (exactly one row via atLeastOne at the call site). Anything else
// (parameters, unresolvable columns, non-literal sides) stays empty for the fallback.
static std::optional<double> getRangeSelectivity(main::ClientContext* context,
    const Expression& predicate,
    const std::unordered_map<common::table_id_t, PlannerTableStats>& tableStats) {
    const auto op = predicate.expressionType;
    if (op != ExpressionType::GREATER_THAN && op != ExpressionType::GREATER_THAN_EQUALS &&
        op != ExpressionType::LESS_THAN && op != ExpressionType::LESS_THAN_EQUALS) {
        return {};
    }
    DASSERT(predicate.getNumChildren() == 2);
    auto lhs = unwrapCast(predicate.getChild(0).get());
    auto rhs = unwrapCast(predicate.getChild(1).get());
    const Expression* propSide = nullptr;
    const Expression* constSide = nullptr;
    auto effectiveOp = op;
    if (isSingleLabelledProperty(*lhs) && rhs->expressionType == ExpressionType::LITERAL) {
        propSide = lhs;
        constSide = rhs;
    } else if (isSingleLabelledProperty(*rhs) && lhs->expressionType == ExpressionType::LITERAL) {
        propSide = rhs;
        constSide = lhs;
        switch (op) {
        case ExpressionType::GREATER_THAN:
            effectiveOp = ExpressionType::LESS_THAN;
            break;
        case ExpressionType::GREATER_THAN_EQUALS:
            effectiveOp = ExpressionType::LESS_THAN_EQUALS;
            break;
        case ExpressionType::LESS_THAN:
            effectiveOp = ExpressionType::GREATER_THAN;
            break;
        default:
            effectiveOp = ExpressionType::GREATER_THAN_EQUALS;
            break;
        }
    } else {
        return {};
    }
    const auto constant = literalToDouble(*constSide);
    const auto minMax = getPropertyMinMax(context, *propSide, tableStats);
    if (!constant.has_value() || !minMax.has_value()) {
        return {};
    }
    const auto [minValue, maxValue] = *minMax;
    const auto c = *constant;
    double selectivity;
    if (minValue >= maxValue) {
        // Single-valued column: the predicate is decided.
        bool keeps = false;
        switch (effectiveOp) {
        case ExpressionType::GREATER_THAN:
            keeps = minValue > c;
            break;
        case ExpressionType::GREATER_THAN_EQUALS:
            keeps = minValue >= c;
            break;
        case ExpressionType::LESS_THAN:
            keeps = minValue < c;
            break;
        default:
            keeps = minValue <= c;
            break;
        }
        selectivity = keeps ? 1.0 : 0.0;
    } else if (effectiveOp == ExpressionType::GREATER_THAN ||
               effectiveOp == ExpressionType::GREATER_THAN_EQUALS) {
        selectivity = (maxValue - c) / (maxValue - minValue);
    } else {
        selectivity = (c - minValue) / (maxValue - minValue);
    }
    return std::min(1.0, std::max(0.0, selectivity));
}

// Upper bound on the number of distinct values of a GROUP BY key: node internal IDs are
// bounded by the node count, single-label properties by the column NDV, and LIST_CREATION
// packs (e.g. Q14's personIdsInPath) by the product of their children's bounds.
std::optional<cardinality_t> CardinalityEstimator::getGroupKeyDomain(const Expression& key) const {
    if (nodeIDName2dom.contains(key.getUniqueName())) {
        return getNodeIDDom(key.getUniqueName());
    }
    if (key.expressionType == ExpressionType::PROPERTY) {
        return getPropertyNumDistinct(context, key, tableStats);
    }
    if (key.expressionType == ExpressionType::FUNCTION &&
        key.constCast<ScalarFunctionExpression>().getFunction().name ==
            function::ListCreationFunction::name) {
        auto product = 1.0;
        for (auto& child : key.getChildren()) {
            auto childDomain = getGroupKeyDomain(*child);
            if (!childDomain.has_value()) {
                return {};
            }
            product *= static_cast<double>(childDomain.value());
            if (product >= static_cast<double>(std::numeric_limits<cardinality_t>::max())) {
                return std::numeric_limits<cardinality_t>::max();
            }
        }
        return static_cast<cardinality_t>(product);
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
        // Ordered comparison against a folded constant uses the observed [min, max]
        // range; everything else (parameters, unresolvable sides) keeps the fixed
        // selectivity.
        if (const auto selectivity = getRangeSelectivity(context, predicate, tableStats)) {
            return atLeastOne(childPlan.getCardinality() * *selectivity);
        }
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

static storage::Table* getTableFromAnyStorageManager(main::ClientContext* context,
    common::table_id_t tableID) {
    auto [cat, sm] = main::DatabaseManager::resolveTableStorage(*context, tableID);
    return sm->getTable(tableID);
}

uint64_t CardinalityEstimator::getNumRels(const Transaction* transaction,
    const std::vector<table_id_t>& tableIDs) const {
    cardinality_t numRels = 0u;
    for (auto tableID : tableIDs) {
        numRels += getTableFromAnyStorageManager(context, tableID)->getNumTotalRows(transaction);
    }
    return atLeastOne(numRels);
}

double CardinalityEstimator::getOneHopExtensionRate(const std::vector<table_id_t>& tableIDs,
    const std::vector<table_id_t>& boundTableIDs, RelDataDirection direction) const {
    // Expected fan-out of a one-hop extend: total incident edges over ALL bound-table rows.
    // (Dividing by only the edge-active bound nodes overestimates full-scan extends: bound
    // rows without edges still produce no output, and the zeros are part of the mean.)
    // Measured ANALYZE degrees feed the numerator exactly; schema-only stats fall back to
    // the same ratio from row counts.
    cardinality_t numBoundNodes = 0;
    cardinality_t numRels = 0;
    bool sawRelStats = false;
    bool allBoundKeysUnique = true;
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
        sawRelStats = true;
        const auto& relStats = stats.relDirectionStats[directionKey].value();
        allBoundKeysUnique &= relStats.boundKeysUnique;
        numRels += relStats.numRows;
    }
    auto rate = static_cast<double>(numRels) / atLeastOne(numBoundNodes);
    return sawRelStats && allBoundKeysUnique ? std::min<double>(rate, 1) : rate;
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

#include "planner/join_order/cardinality_estimator.h"

#include <cmath>
#include <string_view>

#include "binder/expression/literal_expression.h"
#include "binder/expression/property_expression.h"
#include "binder/expression/scalar_function_expression.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/enums/extend_direction_util.h"
#include "common/types/interval_t.h"
#include "function/cast/cast_function_bind_data.h"
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

// Converts a folded literal to double in its type's physical domain (integers, DATE days,
// each timestamp type in its own unit), matching ColumnStats::updateMinMax. Parameters and
// other non-constant expressions stay on the fixed-selectivity path: their values are
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
        // The raw value in the type's own unit; getValue<timestamp_t> accepts TIMESTAMP only.
        return static_cast<double>(value.getValue<int64_t>());
    default:
        return {};
    }
}

// Nanoseconds per unit of the value that ColumnStats::updateMinMax and literalToDouble keep for
// a temporal type; empty for other types.
static std::optional<double> getNanosPerUnit(LogicalTypeID typeID) {
    switch (typeID) {
    case LogicalTypeID::DATE:
        return static_cast<double>(Interval::NANOS_PER_DAY);
    case LogicalTypeID::TIMESTAMP_SEC:
        return static_cast<double>(Interval::NANOS_PER_SEC);
    case LogicalTypeID::TIMESTAMP_MS:
        return static_cast<double>(Interval::NANOS_PER_MSEC);
    case LogicalTypeID::TIMESTAMP:
    case LogicalTypeID::TIMESTAMP_TZ:
        return static_cast<double>(Interval::NANOS_PER_MICRO);
    case LogicalTypeID::TIMESTAMP_NS:
        return 1;
    default:
        // Numeric types. A temporal type that updateMinMax starts tracking must be listed above.
        return {};
    }
}

static bool isCast(const Expression& expr) {
    if (expr.expressionType != ExpressionType::FUNCTION || expr.getNumChildren() != 1) {
        return false;
    }
    return expr.constCast<ScalarFunctionExpression>().getFunction().name.starts_with("CAST");
}

// Strips every one-argument function named CAST* to the underlying property, so a
// binder-inserted cast (an INT32 column vs an INT64 literal compares as CAST(prop) OP literal)
// leaves the property's range. getRangeSelectivity converts temporal units back and accounts for
// a cast to an integer type or to DATE (getCastGrid).
static const Expression* unwrapCast(const Expression* expr) {
    while (isCast(*expr)) {
        expr = expr->getChild(0).get();
    }
    return expr;
}

namespace {

// The grid a cast snaps values to, in the comparison unit: a cast to DATE truncates to the start
// of the day, a cast to an integer type rounds to the nearest integer (ties to even, as FLOAT and
// DOUBLE casts do).
struct CastGrid {
    double step;
    bool rounds;

    double snap(double x) const {
        return (rounds ? std::nearbyint(x / step) : std::floor(x / step)) * step;
    }

    // The x at which `snap(x) OP c` flips: >= and < split the grid at the first point at or above
    // c, > and <= at the first point above it, and rounding reaches a point from half a step below.
    double split(double c, ExpressionType op) const {
        const auto cells = c / step;
        const auto point =
            op == ExpressionType::GREATER_THAN || op == ExpressionType::LESS_THAN_EQUALS ?
                std::floor(cells) + 1 :
                std::ceil(cells);
        return (rounds ? point - 0.5 : point) * step;
    }
};

} // namespace

// The grid of a cast to `type`. Other types count as continuous: nanosecond values pass 2^53 and
// DECIMAL steps such as 0.1 are not exact in double, so a value or constant on a step could land
// one step off.
static std::optional<CastGrid> getTypeGrid(const LogicalType& type, double unitNanos) {
    if (type.getLogicalTypeID() == LogicalTypeID::DATE) {
        return CastGrid{static_cast<double>(Interval::NANOS_PER_DAY) / unitNanos, false};
    }
    if (LogicalTypeUtils::isIntegral(type)) {
        return CastGrid{1, true};
    }
    return {};
}

// A cast to a built-in type. A user function named CAST_*, or any CAST() to a CREATE TYPE type
// (which may call a user CAST_TO_<name>), may do anything to the values.
static bool isBuiltInCast(const Expression& cast) {
    static constexpr std::string_view prefix = "CAST_TO_";
    const auto& funcExpr = cast.constCast<ScalarFunctionExpression>();
    const auto& name = funcExpr.getFunction().name;
    return dynamic_cast<const function::CastFunctionBindData*>(funcExpr.getBindData()) != nullptr &&
           name.starts_with(prefix) && LogicalType::isBuiltInType(name.substr(prefix.size()));
}

// The coarsest grid among the casts above the property. Empty when the property's own values
// already lie on it (no cast, widening, integer narrowing), or when the chain holds a cast that is
// not built in. A DECIMAL cast below an integer cast rounds first, which the grid ignores: a value
// within half a DECIMAL step of a .5 boundary can land on the other side.
static std::optional<CastGrid> getCastGrid(const Expression* expr, double unitNanos) {
    std::optional<CastGrid> grid;
    for (; isCast(*expr); expr = expr->getChild(0).get()) {
        if (!isBuiltInCast(*expr)) {
            return {};
        }
        const auto castGrid = getTypeGrid(expr->getDataType(), unitNanos);
        if (castGrid.has_value() && (!grid.has_value() || castGrid->step > grid->step)) {
            grid = castGrid;
        }
    }
    const auto propGrid = getTypeGrid(expr->getDataType(), unitNanos);
    if (!grid.has_value() || (propGrid.has_value() && propGrid->step >= grid->step)) {
        return {};
    }
    return grid;
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
    const Expression* propWithCasts = nullptr;
    const Expression* constSide = nullptr;
    auto effectiveOp = op;
    if (isSingleLabelledProperty(*lhs) && rhs->expressionType == ExpressionType::LITERAL) {
        propSide = lhs;
        propWithCasts = predicate.getChild(0).get();
        constSide = rhs;
    } else if (isSingleLabelledProperty(*rhs) && lhs->expressionType == ExpressionType::LITERAL) {
        propSide = rhs;
        propWithCasts = predicate.getChild(1).get();
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
    // Stripping a cast between temporal types leaves the range and the constant in different
    // units; both go to the finer one by a whole factor (1 for the same type). No cast converts
    // between temporal and numeric types, but a user function named CAST_* that unwrapCast
    // strips can, in an unknown unit, so that falls back to the fixed selectivity.
    const auto propNanos = getNanosPerUnit(propSide->getDataType().getLogicalTypeID());
    const auto constNanos = getNanosPerUnit(constSide->getDataType().getLogicalTypeID());
    if (propNanos.has_value() != constNanos.has_value()) {
        return {};
    }
    // Both are set (temporal) or both empty (numeric, factor 1).
    const auto unitNanos = std::min(propNanos.value_or(1), constNanos.value_or(1));
    const auto propScale = propNanos.value_or(1) / unitNanos;
    const auto constScale = constNanos.value_or(1) / unitNanos;
    const auto minValue = minMax->first * propScale;
    const auto maxValue = minMax->second * propScale;
    const auto c = *constant * constScale;
    const auto grid = getCastGrid(propWithCasts, unitNanos);
    double selectivity;
    if (minValue >= maxValue) {
        // Single-valued column: the predicate is decided on the value after the cast.
        const auto value = grid.has_value() ? grid->snap(minValue) : minValue;
        bool keeps = false;
        switch (effectiveOp) {
        case ExpressionType::GREATER_THAN:
            keeps = value > c;
            break;
        case ExpressionType::GREATER_THAN_EQUALS:
            keeps = value >= c;
            break;
        case ExpressionType::LESS_THAN:
            keeps = value < c;
            break;
        default:
            keeps = value <= c;
            break;
        }
        selectivity = keeps ? 1.0 : 0.0;
    } else {
        // A cast that snaps the values moves the split from c to where the cast reaches it.
        const auto split = grid.has_value() ? grid->split(c, effectiveOp) : c;
        if (effectiveOp == ExpressionType::GREATER_THAN ||
            effectiveOp == ExpressionType::GREATER_THAN_EQUALS) {
            selectivity = (maxValue - split) / (maxValue - minValue);
        } else {
            selectivity = (split - minValue) / (maxValue - minValue);
        }
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

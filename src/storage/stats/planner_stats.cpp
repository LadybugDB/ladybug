#include "storage/stats/planner_stats.h"

#include "catalog/catalog.h"
#include "catalog/catalog_entry/index_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/constants.h"
#include "common/enums/extend_direction_util.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "storage/table/rel_table.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::transaction;

namespace lbug {
namespace storage {

static PlannerRelDirectionStats computeRelDirectionStats(RelTable& relTable,
    const Transaction* transaction, RelDataDirection direction) {
    auto degreeEntries = relTable.getDegreeEntries(transaction, direction);
    cardinality_t totalDegree = 0;
    cardinality_t maxDegree = 0;
    for (const auto& [_, degree] : degreeEntries) {
        totalDegree += degree;
        maxDegree = std::max<cardinality_t>(maxDegree, degree);
    }
    auto stats = PlannerRelDirectionStats{};
    stats.numRows = relTable.getNumTotalRows(transaction);
    stats.numActiveBoundNodes = degreeEntries.size();
    stats.maxDegree = maxDegree;
    stats.avgDegree =
        degreeEntries.empty() ? 0 : static_cast<double>(totalDegree) / degreeEntries.size();
    stats.boundKeysUnique = maxDegree <= 1;
    return stats;
}

static void appendIndexStats(PlannerTableStats& plannerStats, const Catalog& catalog,
    const Transaction* transaction, const TableCatalogEntry& tableEntry, table_id_t tableID) {
    for (const auto indexEntry : catalog.getIndexEntries(transaction, tableID)) {
        auto indexStats = PlannerIndexStats{};
        indexStats.indexType = indexEntry->getIndexType();
        indexStats.isPrimary = indexEntry->getIndexName() == InternalKeyword::ID;
        for (const auto propertyID : indexEntry->getPropertyIDs()) {
            indexStats.columnIDs.push_back(tableEntry.getColumnID(propertyID));
        }
        plannerStats.indexStats.push_back(std::move(indexStats));
    }
}

PlannerTableStats buildPlannerTableStats(StorageManager& storageManager, const Catalog& catalog,
    const Transaction* transaction, const TableCatalogEntry& tableEntry,
    table_id_t physicalTableID) {
    auto tableID = physicalTableID == INVALID_TABLE_ID ? tableEntry.getTableID() : physicalTableID;
    auto* table = storageManager.getTable(tableID);
    auto plannerStats = PlannerTableStats{};
    plannerStats.tableID = tableID;
    plannerStats.tableType = tableEntry.getTableType();
    plannerStats.tableChangeEpoch = table->getChangeEpoch();
    switch (tableEntry.getTableType()) {
    case TableType::NODE: {
        plannerStats.storageStats = table->cast<NodeTable>().getStats(transaction);
    } break;
    case TableType::REL: {
        auto& relGroupEntry = tableEntry.constCast<RelGroupCatalogEntry>();
        auto* relTable = table->ptrCast<RelTable>();
        for (const auto direction : relGroupEntry.getRelDataDirections()) {
            const auto directionKey = RelDirectionUtils::relDirectionToKeyIdx(direction);
            plannerStats.relDirectionStats[directionKey] =
                computeRelDirectionStats(*relTable, transaction, direction);
        }
    } break;
    default: {
        UNREACHABLE_CODE;
    }
    }
    appendIndexStats(plannerStats, catalog, transaction, tableEntry, tableID);
    return plannerStats;
}

std::vector<table_id_t> analyzePlannerStats(StorageManager& storageManager, const Catalog& catalog,
    const Transaction* transaction, const std::optional<std::string>& tableName) {
    std::vector<table_id_t> analyzedTables;
    auto analyzeTable = [&](const TableCatalogEntry& tableEntry) {
        if (tableEntry.getType() == CatalogEntryType::FOREIGN_TABLE_ENTRY) {
            return;
        }
        if (tableEntry.getTableType() == TableType::REL) {
            auto& relGroupEntry = tableEntry.constCast<RelGroupCatalogEntry>();
            for (const auto& relInfo : relGroupEntry.getRelEntryInfos()) {
                storageManager.setCachedPlannerTableStats(buildPlannerTableStats(storageManager,
                    catalog, transaction, tableEntry, relInfo.oid));
                analyzedTables.push_back(relInfo.oid);
            }
            return;
        }
        storageManager.setCachedPlannerTableStats(
            buildPlannerTableStats(storageManager, catalog, transaction, tableEntry));
        analyzedTables.push_back(tableEntry.getTableID());
    };
    if (tableName.has_value()) {
        analyzeTable(*catalog.getTableCatalogEntry(transaction, tableName.value(), false));
        return analyzedTables;
    }
    for (const auto tableEntry : catalog.getTableEntries(transaction, false /* useInternal */)) {
        analyzeTable(*tableEntry);
    }
    return analyzedTables;
}

} // namespace storage
} // namespace lbug

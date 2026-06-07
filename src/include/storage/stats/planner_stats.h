#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "common/enums/rel_direction.h"
#include "common/enums/table_type.h"
#include "common/types/types.h"
#include "storage/stats/table_stats.h"

namespace lbug {
namespace catalog {
class Catalog;
class TableCatalogEntry;
} // namespace catalog
namespace transaction {
class Transaction;
} // namespace transaction

namespace storage {
class StorageManager;

enum class PlannerStatsMode : uint8_t { SCHEMA_ONLY, ANALYZE };

struct PlannerIndexStats {
    std::string indexType;
    std::vector<common::column_id_t> columnIDs;
    bool isPrimary = false;
};

struct PlannerRelDirectionStats {
    common::cardinality_t numRows = 0;
    common::cardinality_t numActiveBoundNodes = 0;
    common::cardinality_t maxDegree = 0;
    double avgDegree = 0;
    bool boundKeysUnique = false;
};

struct PlannerTableStats {
    common::table_id_t tableID = common::INVALID_TABLE_ID;
    common::TableType tableType = common::TableType::NODE;
    uint64_t tableChangeEpoch = 0;
    std::optional<TableStats> storageStats;
    std::array<std::optional<PlannerRelDirectionStats>, common::NUM_REL_DIRECTIONS>
        relDirectionStats;
    std::vector<PlannerIndexStats> indexStats;

    PlannerTableStats copy() const {
        auto result = PlannerTableStats{};
        result.tableID = tableID;
        result.tableType = tableType;
        result.tableChangeEpoch = tableChangeEpoch;
        if (storageStats.has_value()) {
            result.storageStats = storageStats->copy();
        }
        result.relDirectionStats = relDirectionStats;
        result.indexStats = indexStats;
        return result;
    }
};

PlannerTableStats buildPlannerTableStats(StorageManager& storageManager,
    const catalog::Catalog& catalog, const transaction::Transaction* transaction,
    const catalog::TableCatalogEntry& tableEntry,
    common::table_id_t physicalTableID = common::INVALID_TABLE_ID,
    PlannerStatsMode mode = PlannerStatsMode::ANALYZE);

std::vector<common::table_id_t> analyzePlannerStats(StorageManager& storageManager,
    const catalog::Catalog& catalog, const transaction::Transaction* transaction,
    const std::optional<std::string>& tableName);

} // namespace storage
} // namespace lbug

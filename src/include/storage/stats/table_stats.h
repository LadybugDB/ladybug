#pragma once

#include "common/types/types.h"
#include "storage/stats/column_stats.h"

namespace lbug::common {
class LogicalType;
}
namespace lbug {
namespace storage {

class TableStats {
public:
    explicit TableStats(std::span<const common::LogicalType> dataTypes);

    EXPLICIT_COPY_DEFAULT_MOVE(TableStats);

    void incrementCardinality(common::cardinality_t increment) { cardinality += increment; }

    void merge(const TableStats& other) {
        std::vector<common::column_id_t> columnIDs;
        for (auto i = 0u; i < columnStats.size(); i++) {
            columnIDs.push_back(i);
        }
        merge(columnIDs, other);
    }

    void merge(const std::vector<common::column_id_t>& columnIDs, const TableStats& other) {
        cardinality += other.cardinality;
        DASSERT(columnIDs.size() == other.columnStats.size());
        for (auto i = 0u; i < columnIDs.size(); ++i) {
            auto columnID = columnIDs[i];
            DASSERT(columnID < columnStats.size());
            columnStats[columnID].merge(other.columnStats[i]);
        }
    }

    common::cardinality_t getTableCard() const { return cardinality; }

    common::cardinality_t getNumDistinctValues(common::column_id_t columnID) const {
        DASSERT(columnID < columnStats.size());
        return columnStats[columnID].getNumDistinctValues();
    }

    // Observed [min, max] of a column, for range-selectivity estimation. Empty when the
    // column saw no non-null values or its type is not min/max-tracked.
    std::optional<std::pair<double, double>> getColumnMinMax(common::column_id_t columnID) const {
        DASSERT(columnID < columnStats.size());
        const auto& min = columnStats[columnID].getMinValue();
        const auto& max = columnStats[columnID].getMaxValue();
        if (!min.has_value() || !max.has_value()) {
            return {};
        }
        return std::make_pair(*min, *max);
    }

    void update(const std::vector<common::ValueVector*>& vectors,
        size_t numColumns = std::numeric_limits<size_t>::max());
    void update(const std::vector<common::column_id_t>& columnIDs,
        const std::vector<common::ValueVector*>& vectors,
        size_t numColumns = std::numeric_limits<size_t>::max());

    ColumnStats& addNewColumn(const common::LogicalType& dataType) {
        columnStats.emplace_back(dataType);
        return columnStats.back();
    }

    void serialize(common::Serializer& serializer) const;
    TableStats deserialize(common::Deserializer& deserializer);

private:
    TableStats(const TableStats& other);

private:
    // Note: cardinality is the estimated number of rows in the table. It is not always up-to-date.
    common::cardinality_t cardinality;
    std::vector<ColumnStats> columnStats;
};

} // namespace storage
} // namespace lbug

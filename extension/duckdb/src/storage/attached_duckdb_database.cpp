#include "storage/attached_duckdb_database.h"

#include "duckdb.hpp"

namespace lbug {
namespace duckdb_extension {

std::vector<std::string> AttachedDuckDBDatabase::getTableColumnNames(
    const std::string& tableName) const {
    std::string query =
        common::stringFormat("SELECT column_name FROM information_schema.columns WHERE "
                             "table_name = '{}' ORDER BY ordinal_position",
            tableName);

    auto result = connector->executeQuery(query);
    if (!result || result->RowCount() == 0) {
        return {};
    }

    std::vector<std::string> columnNames;
    for (auto i = 0u; i < result->RowCount(); i++) {
        columnNames.push_back(result->GetValue(0, i).GetValue<std::string>());
    }
    return columnNames;
}

} // namespace duckdb_extension
} // namespace lbug

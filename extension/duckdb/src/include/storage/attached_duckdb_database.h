#pragma once

#include "connector/duckdb_connector.h"
#include "main/attached_database.h"

namespace lbug {
namespace duckdb_extension {

class AttachedDuckDBDatabase : public main::AttachedDatabase {
public:
    AttachedDuckDBDatabase(std::string dbName, std::string dbType,
        std::unique_ptr<extension::CatalogExtension> catalog,
        std::unique_ptr<DuckDBConnector> connector)
        : main::AttachedDatabase{std::move(dbName), std::move(dbType), std::move(catalog)},
          connector{std::move(connector)} {}

    const DuckDBConnector& getConnector() const { return *connector; }

    std::unique_ptr<duckdb::MaterializedQueryResult> executeQuery(
        const std::string& query) const override {
        return connector->executeQuery(query);
    }

    std::vector<std::string> getTableColumnNames(const std::string& tableName) const override {
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

protected:
    std::unique_ptr<DuckDBConnector> connector;
};

} // namespace duckdb_extension
} // namespace lbug

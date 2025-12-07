#include "catalog/duckdb_table_catalog_entry.h"

#include "binder/bound_scan_source.h"
#include "binder/expression/variable_expression.h"
#include "common/constants.h"
#include "function/duckdb_scan.h"

namespace lbug {
namespace catalog {

DuckDBTableCatalogEntry::DuckDBTableCatalogEntry(std::string name,
    function::TableFunction scanFunction,
    std::shared_ptr<duckdb_extension::DuckDBTableScanInfo> scanInfo)
    : TableCatalogEntry{CatalogEntryType::FOREIGN_TABLE_ENTRY, std::move(name)},
      scanFunction{std::move(scanFunction)}, scanInfo{std::move(scanInfo)} {}

common::TableType DuckDBTableCatalogEntry::getTableType() const {
    return common::TableType::FOREIGN;
}

std::unique_ptr<binder::BoundTableScanInfo> DuckDBTableCatalogEntry::getBoundScanInfo(
    main::ClientContext* context, const std::string& nodeUniqueName) {
    auto columnNames = scanInfo->getColumnNames();
    auto columnTypes = scanInfo->getColumnTypes(*context);
    binder::expression_vector columns;

    // Add rowid as _ID (internal ID) if nodeUniqueName is provided
    if (!nodeUniqueName.empty()) {
        auto idUniqueName = nodeUniqueName + "." + std::string(common::InternalKeyword::ID);
        columns.push_back(std::make_shared<binder::VariableExpression>(common::LogicalType::INT64(),
            idUniqueName, "rowid"));
    }

    for (auto i = 0u; i < columnNames.size(); i++) {
        std::string uniqueName = columnNames[i];
        if (!nodeUniqueName.empty()) {
            uniqueName = nodeUniqueName + "." + columnNames[i];
        }
        columns.push_back(std::make_shared<binder::VariableExpression>(std::move(columnTypes[i]),
            uniqueName, columnNames[i]));
    }

    // Build column names for DuckDB query - include rowid if needed
    std::vector<std::string> duckdbColumnNames;
    if (!nodeUniqueName.empty()) {
        duckdbColumnNames.push_back("rowid");
    }
    duckdbColumnNames.insert(duckdbColumnNames.end(), columnNames.begin(), columnNames.end());

    auto bindData =
        std::make_unique<duckdb_extension::DuckDBScanBindData>(scanInfo->getTemplateQuery(*context),
            duckdbColumnNames, scanInfo->getConnector(), std::move(columns));
    return std::make_unique<binder::BoundTableScanInfo>(scanFunction, std::move(bindData));
}

std::unique_ptr<TableCatalogEntry> DuckDBTableCatalogEntry::copy() const {
    auto other = std::make_unique<DuckDBTableCatalogEntry>(name, scanFunction, scanInfo);
    other->copyFrom(*this);
    return other;
}

std::unique_ptr<binder::BoundExtraCreateCatalogEntryInfo>
DuckDBTableCatalogEntry::getBoundExtraCreateInfo(transaction::Transaction*) const {
    KU_UNREACHABLE;
}

} // namespace catalog
} // namespace lbug

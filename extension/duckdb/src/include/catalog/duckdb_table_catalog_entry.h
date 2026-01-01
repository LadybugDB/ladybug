#pragma once

#include <optional>

#include "catalog/catalog_entry/table_catalog_entry.h"
#include "function/duckdb_scan.h"
#include "function/table/table_function.h"

namespace lbug {
namespace catalog {

class DuckDBTableCatalogEntry final : public TableCatalogEntry {
public:
    //===--------------------------------------------------------------------===//
    // constructors
    //===--------------------------------------------------------------------===//
    DuckDBTableCatalogEntry(std::string name, std::optional<function::TableFunction> scanFunction,
        std::shared_ptr<duckdb_extension::DuckDBTableScanInfo> scanInfo);

    //===--------------------------------------------------------------------===//
    // getter & setter
    //===--------------------------------------------------------------------===//
    common::TableType getTableType() const override;
    std::optional<function::TableFunction> getScanFunction() const override { return scanFunction; }
    std::unique_ptr<binder::BoundTableScanInfo> getBoundScanInfo(main::ClientContext* context,
        const std::string& nodeUniqueName = "") override;

    //===--------------------------------------------------------------------===//
    // serialization & deserialization
    //===--------------------------------------------------------------------===//
    std::unique_ptr<TableCatalogEntry> copy() const override;

private:
    std::unique_ptr<binder::BoundExtraCreateCatalogEntryInfo> getBoundExtraCreateInfo(
        transaction::Transaction* transaction) const override;

private:
    std::optional<function::TableFunction> scanFunction;
    std::shared_ptr<duckdb_extension::DuckDBTableScanInfo> scanInfo;
};

} // namespace catalog
} // namespace lbug

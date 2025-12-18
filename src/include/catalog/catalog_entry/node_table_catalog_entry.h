#pragma once

#include <functional>
#include <optional>

#include "function/table/table_function.h"
#include "table_catalog_entry.h"

namespace lbug {
namespace transaction {
class Transaction;
} // namespace transaction

namespace catalog {

// Callback to create bind data for foreign tables
// This allows extensions to provide bind data creation without core needing to know extension types
using CreateBindDataFunc =
    std::function<std::unique_ptr<function::TableFuncBindData>(main::ClientContext* context)>;

// Tag for shadow table constructor
struct ShadowTag {};

class Catalog;
class LBUG_API NodeTableCatalogEntry final : public TableCatalogEntry {
    static constexpr CatalogEntryType entryType_ = CatalogEntryType::NODE_TABLE_ENTRY;

public:
    NodeTableCatalogEntry() = default;
    NodeTableCatalogEntry(std::string name, std::string primaryKeyName, std::string storage = "")
        : TableCatalogEntry{entryType_, std::move(name)}, primaryKeyName{std::move(primaryKeyName)},
          storage{std::move(storage)} {}

    // Constructor for foreign-backed tables
    NodeTableCatalogEntry(std::string name, std::string primaryKeyName,
        function::TableFunction scanFunction, CreateBindDataFunc createBindData,
        std::string foreignDatabaseName = "")
        : TableCatalogEntry{entryType_, std::move(name)}, primaryKeyName{std::move(primaryKeyName)},
          scanFunction{std::move(scanFunction)}, createBindDataFunc{std::move(createBindData)},
          foreignDatabaseName{std::move(foreignDatabaseName)} {}

    // Constructor for shadow tables
    NodeTableCatalogEntry(std::string name, std::string primaryKeyName,
        std::string foreignDatabaseName, ShadowTag)
        : TableCatalogEntry{entryType_, std::move(name)}, primaryKeyName{std::move(primaryKeyName)},
          foreignDatabaseName{std::move(foreignDatabaseName)} {}

    bool isParent(common::table_id_t /*tableID*/) override { return false; }
    common::TableType getTableType() const override { return common::TableType::NODE; }

    std::string getPrimaryKeyName() const { return primaryKeyName; }
    common::property_id_t getPrimaryKeyID() const {
        return propertyCollection.getPropertyID(primaryKeyName);
    }
    const binder::PropertyDefinition& getPrimaryKeyDefinition() const {
        return getProperty(primaryKeyName);
    }
    const std::string& getStorage() const { return storage; }
    const std::optional<function::TableFunction>& getScanFunctionOpt() const {
        return scanFunction;
    }
    const CreateBindDataFunc& getCreateBindDataFunc() const { return createBindDataFunc; }
    const std::string& getForeignDatabaseName() const { return foreignDatabaseName; }

    void setReferencedEntry(TableCatalogEntry* entry) { referencedEntry = entry; }
    TableCatalogEntry* getReferencedEntry() const { return referencedEntry; }
    void setForeignDatabaseName(std::string s) { foreignDatabaseName = std::move(s); }

    function::TableFunction getScanFunction() override;
    std::unique_ptr<binder::BoundTableScanInfo> getBoundScanInfo(main::ClientContext* context,
        const std::string& nodeUniqueName = "") override;

    void renameProperty(const std::string& propertyName, const std::string& newName) override;

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<NodeTableCatalogEntry> deserialize(common::Deserializer& deserializer);

    std::unique_ptr<TableCatalogEntry> copy() const override;
    std::string toCypher(const ToCypherInfo& info) const override;

private:
    std::unique_ptr<binder::BoundExtraCreateCatalogEntryInfo> getBoundExtraCreateInfo(
        transaction::Transaction* transaction) const override;

private:
    std::string primaryKeyName;
    std::string storage;
    std::optional<function::TableFunction> scanFunction;
    CreateBindDataFunc createBindDataFunc; // Callback to create bind data
    std::string foreignDatabaseName;
    TableCatalogEntry* referencedEntry = nullptr;
};

} // namespace catalog
} // namespace lbug

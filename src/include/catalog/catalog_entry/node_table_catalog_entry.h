#pragma once

#include <functional>
#include <optional>

#include "common/enums/storage_format.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "common/string_utils.h"
#include "function/table/table_function.h"
#include "table_catalog_entry.h"

namespace lbug {
namespace transaction {
class Transaction;
} // namespace transaction

namespace catalog {

struct SortedByProperty {
    std::string propertyName;
    bool ascending;

    void serialize(common::Serializer& serializer) const;
    static SortedByProperty deserialize(common::Deserializer& deserializer);
};

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
    NodeTableCatalogEntry(std::string name, std::string primaryKeyName, std::string storage = "",
        common::StorageFormat storageFormat = common::StorageFormat::NONE)
        : TableCatalogEntry{entryType_, std::move(name)}, primaryKeyName{std::move(primaryKeyName)},
          storage{std::move(storage)}, storageFormat{storageFormat} {}

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
    common::StorageFormat getStorageFormat() const { return storageFormat; }
    const std::vector<SortedByProperty>& getSortedByProperties() const {
        return sortedByProperties;
    }
    bool isLeadingSortPrimaryKeyAsc() const {
        return !sortedByProperties.empty() && sortedByProperties[0].ascending &&
               common::StringUtils::caseInsensitiveEquals(sortedByProperties[0].propertyName,
                   primaryKeyName);
    }
    void setSortedByProperties(std::vector<SortedByProperty> properties) {
        sortedByProperties = std::move(properties);
    }
    std::optional<function::TableFunction> getScanFunction() const override;
    const CreateBindDataFunc& getCreateBindDataFunc() const { return createBindDataFunc; }
    const std::string& getForeignDatabaseName() const { return foreignDatabaseName; }

    void setReferencedEntry(TableCatalogEntry* entry) { referencedEntry = entry; }
    TableCatalogEntry* getReferencedEntry() const { return referencedEntry; }
    void setForeignDatabaseName(std::string s) { foreignDatabaseName = std::move(s); }

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
    std::vector<SortedByProperty> sortedByProperties;
    std::string storage;
    common::StorageFormat storageFormat = common::StorageFormat::NONE;
    std::optional<function::TableFunction> scanFunction;
    CreateBindDataFunc createBindDataFunc; // Callback to create bind data
    std::string foreignDatabaseName;
    TableCatalogEntry* referencedEntry = nullptr;
};

} // namespace catalog
} // namespace lbug

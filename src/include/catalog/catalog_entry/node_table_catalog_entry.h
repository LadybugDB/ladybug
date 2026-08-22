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

// Callback to create bind data for foreign tables.
// `nodeUniqueName` is the unique expression name of the node pattern being bound (empty for
// standalone function binds). Implementations must name their output columns
// "<nodeUniqueName>.<property>" and "<nodeUniqueName>._ID" so planner schema lookups on node
// property expressions resolve against the scan output.
using CreateBindDataFunc = std::function<std::unique_ptr<function::TableFuncBindData>(
    main::ClientContext* context, const std::string& nodeUniqueName)>;

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

    bool isParent(common::table_id_t /*tableID*/) override { return isPartitioned(); }
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
    // CSR is an explicit declaration that the primary key is a csr_index interchangeable
    // with the rel table's table_offset (i.e. primary_key == rowid). It is only honored
    // while the node table has not been mutated since the CSR declaration.
    bool isCsr() const { return csr; }
    uint64_t getCsrChangeEpoch() const { return csrChangeEpoch; }
    void setCsr(bool enable, uint64_t changeEpoch) {
        csr = enable;
        csrChangeEpoch = enable ? changeEpoch : 0;
    }
    void setSortedByProperties(std::vector<SortedByProperty> properties) {
        sortedByProperties = std::move(properties);
    }
    std::optional<function::TableFunction> getScanFunction() const override { return scanFunction; }
    void setScanFunction(function::TableFunction scanFunction_) {
        scanFunction = std::move(scanFunction_);
    }
    void setCreateBindDataFunc(CreateBindDataFunc createBindDataFunc_) {
        createBindDataFunc = std::move(createBindDataFunc_);
    }
    const std::string& getForeignDatabaseName() const { return foreignDatabaseName; }

    void setReferencedEntry(TableCatalogEntry* entry) { referencedEntry = entry; }
    TableCatalogEntry* getReferencedEntry() const { return referencedEntry; }
    void setForeignDatabaseName(std::string s) { foreignDatabaseName = std::move(s); }

    // ---- PostgreSQL-style partitioning ----
    // A partitioned node table is a logical parent that does not own physical storage; each
    // partition is a separate node-table subgraph that does. This mirrors how we plan to expose
    // local (in-process) or remote (columnar protocol such as ADBC) partitions uniformly.
    bool isPartitioned() const { return partitionMethod.has_value(); }
    bool isPartitionChild() const { return parentTableID != common::INVALID_TABLE_ID; }
    void setPartitionInfo(binder::BoundPartitionMethod method, std::string columnName,
        common::property_id_t columnID, uint64_t numPartitions);
    void addChildTableID(common::table_id_t tableID) { childTableIDs.push_back(tableID); }
    void setParentInfo(common::table_id_t parentTableID_, uint64_t partitionIndex_) {
        parentTableID = parentTableID_;
        partitionIndex = partitionIndex_;
    }
    std::optional<binder::BoundPartitionMethod> getPartitionMethod() const {
        return partitionMethod;
    }
    const std::string& getPartitionColumnName() const { return partitionColumnName; }
    common::property_id_t getPartitionColumnID() const { return partitionColumnID; }
    uint64_t getNumPartitions() const { return numPartitions; }
    const std::vector<common::table_id_t>& getChildTableIDs() const { return childTableIDs; }
    common::table_id_t getParentTableID() const { return parentTableID; }
    uint64_t getPartitionIndex() const { return partitionIndex; }

    // ---- LIST partitioning ----
    // A LIST parent keeps one partition per distinct partition-key value. The value -> partition
    // mapping is persisted as an opaque encoded key (see processor-side encodeListPartitionKey)
    // so the catalog does not need typed Value serialization.
    std::optional<common::table_id_t> findListPartition(
        const std::string& encodedKey) const; // linear scan; callers cache hot lookups
    void addListPartition(std::string encodedKey, common::table_id_t tableID);
    const std::vector<std::pair<std::string, common::table_id_t>>& getListPartitionKeys() const {
        return listPartitionKeys;
    }

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
    bool csr = false;
    uint64_t csrChangeEpoch = 0;
    std::string storage;
    common::StorageFormat storageFormat = common::StorageFormat::NONE;
    std::optional<function::TableFunction> scanFunction;
    CreateBindDataFunc createBindDataFunc; // Callback to create bind data
    std::string foreignDatabaseName;
    TableCatalogEntry* referencedEntry = nullptr;

    // Partitioning state. `partitionMethod` is set only on the logical parent. `parentTableID` /
    // `partitionIndex` are set only on child partition subgraphs.
    std::optional<binder::BoundPartitionMethod> partitionMethod;
    std::string partitionColumnName;
    common::property_id_t partitionColumnID = common::INVALID_PROPERTY_ID;
    uint64_t numPartitions = 0;
    std::vector<common::table_id_t> childTableIDs;
    // LIST-only, parent side: encoded partition-key value -> child table id, in creation order.
    std::vector<std::pair<std::string, common::table_id_t>> listPartitionKeys;
    common::table_id_t parentTableID = common::INVALID_TABLE_ID;
    uint64_t partitionIndex = 0;
};

} // namespace catalog
} // namespace lbug

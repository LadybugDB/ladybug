#pragma once

#include <optional>

#include "catalog/catalog_entry/catalog_entry_type.h"
#include "catalog/catalog_entry/node_table_id_pair.h"
#include "common/enums/conflict_action.h"
#include "common/enums/extend_direction.h"
#include "common/enums/rel_multiplicity.h"
#include "common/enums/storage_format.h"
#include "function/table/bind_data.h"
#include "function/table/table_function.h"
#include "property_definition.h"

namespace lbug {
namespace common {
enum class RelMultiplicity : uint8_t;
}
namespace binder {
struct BoundExtraCreateCatalogEntryInfo {
    virtual ~BoundExtraCreateCatalogEntryInfo() = default;

    template<class TARGET>
    const TARGET* constPtrCast() const {
        return common::dynamic_cast_checked<const TARGET*>(this);
    }

    template<class TARGET>
    TARGET* ptrCast() {
        return common::dynamic_cast_checked<TARGET*>(this);
    }

    virtual inline std::unique_ptr<BoundExtraCreateCatalogEntryInfo> copy() const = 0;
};

struct BoundCreateTableInfo {
    catalog::CatalogEntryType type = catalog::CatalogEntryType::DUMMY_ENTRY;
    std::string tableName;
    common::ConflictAction onConflict = common::ConflictAction::INVALID;
    std::unique_ptr<BoundExtraCreateCatalogEntryInfo> extraInfo;
    bool isInternal = false;
    bool hasParent = false;

    BoundCreateTableInfo() = default;
    BoundCreateTableInfo(catalog::CatalogEntryType type, std::string tableName,
        common::ConflictAction onConflict,
        std::unique_ptr<BoundExtraCreateCatalogEntryInfo> extraInfo, bool isInternal,
        bool hasParent = false)
        : type{type}, tableName{std::move(tableName)}, onConflict{onConflict},
          extraInfo{std::move(extraInfo)}, isInternal{isInternal}, hasParent{hasParent} {}
    EXPLICIT_COPY_DEFAULT_MOVE(BoundCreateTableInfo);

    std::string toString() const;

private:
    BoundCreateTableInfo(const BoundCreateTableInfo& other)
        : type{other.type}, tableName{other.tableName}, onConflict{other.onConflict},
          extraInfo{other.extraInfo->copy()}, isInternal{other.isInternal},
          hasParent{other.hasParent} {}
};

struct LBUG_API BoundExtraCreateTableInfo : BoundExtraCreateCatalogEntryInfo {
    std::vector<PropertyDefinition> propertyDefinitions;

    explicit BoundExtraCreateTableInfo(std::vector<PropertyDefinition> propertyDefinitions)
        : propertyDefinitions{std::move(propertyDefinitions)} {}

    BoundExtraCreateTableInfo(const BoundExtraCreateTableInfo& other)
        : BoundExtraCreateTableInfo{copyVector(other.propertyDefinitions)} {}
    BoundExtraCreateTableInfo& operator=(const BoundExtraCreateTableInfo&) = delete;

    std::unique_ptr<BoundExtraCreateCatalogEntryInfo> copy() const override {
        return std::make_unique<BoundExtraCreateTableInfo>(*this);
    }
};

// PostgreSQL-style partitioning method applied to a node table. LIST partitions dynamically: one
// partition per distinct partition-key value, created on demand (numPartitions is unused).
enum class BoundPartitionMethod : uint8_t { HASH = 0, RANGE = 1, LIST = 2 };

struct BoundPartitionInfo {
    BoundPartitionMethod method;
    std::string columnName;
    uint64_t numPartitions;
    // Older databases allowed rows in arbitrary child partitions. Only new parents have
    // enforced routing constraints; preserve this flag when reconstructing a parent from WAL.
    bool partitionPruningSafe = true;

    BoundPartitionInfo(BoundPartitionMethod method, std::string columnName, uint64_t numPartitions)
        : method{method}, columnName{std::move(columnName)}, numPartitions{numPartitions} {}
};

struct BoundExtraCreateNodeTableInfo final : BoundExtraCreateTableInfo {
    std::string primaryKeyName;
    std::string storage;
    common::StorageFormat storageFormat = common::StorageFormat::NONE;
    // External format name for StorageFormat::EXTERNAL (e.g. "lance"); empty otherwise.
    std::string storageFormatName;
    std::optional<BoundPartitionInfo> partitionInfo;
    // Set when this entry is a dynamically created LIST partition: links the child to its
    // parent and ordinal so catalog creation (including WAL replay) registers it as a
    // partition subgraph.
    common::table_id_t partitionParentTableID = common::INVALID_TABLE_ID;
    uint64_t partitionChildIndex = 0;

    void setPartitionParent(common::table_id_t parentID, uint64_t index) {
        partitionParentTableID = parentID;
        partitionChildIndex = index;
    }

    // Encoded LIST key for a dynamically created LIST partition (empty otherwise). Carried
    // into the child entry so WAL replay can restore the parent's key -> child map.
    std::string listPartitionKey;
    // WAL replay only: the recorded parent's children each carry their own create record,
    // so replay must not recreate the initial partition set implicitly.
    bool skipInitialPartitionCreation = false;

    void setListPartitionKey(std::string key) { listPartitionKey = std::move(key); }

    BoundExtraCreateNodeTableInfo(std::string primaryKeyName,
        std::vector<PropertyDefinition> definitions, std::string storage = "",
        common::StorageFormat storageFormat = common::StorageFormat::NONE,
        std::optional<BoundPartitionInfo> partitionInfo = std::nullopt,
        std::string storageFormatName = "")
        : BoundExtraCreateTableInfo{std::move(definitions)},
          primaryKeyName{std::move(primaryKeyName)}, storage{std::move(storage)},
          storageFormat{storageFormat}, storageFormatName{std::move(storageFormatName)},
          partitionInfo{std::move(partitionInfo)} {}
    BoundExtraCreateNodeTableInfo(const BoundExtraCreateNodeTableInfo& other)
        : BoundExtraCreateTableInfo{copyVector(other.propertyDefinitions)},
          primaryKeyName{other.primaryKeyName}, storage{other.storage},
          storageFormat{other.storageFormat}, storageFormatName{other.storageFormatName},
          partitionInfo{other.partitionInfo}, partitionParentTableID{other.partitionParentTableID},
          partitionChildIndex{other.partitionChildIndex}, listPartitionKey{other.listPartitionKey},
          skipInitialPartitionCreation{other.skipInitialPartitionCreation} {}

    std::unique_ptr<BoundExtraCreateCatalogEntryInfo> copy() const override {
        return std::make_unique<BoundExtraCreateNodeTableInfo>(*this);
    }
};

struct BoundRelTableInfo {
    catalog::NodeTableIDPair nodePair;
    common::RelMultiplicity srcMultiplicity;
    common::RelMultiplicity dstMultiplicity;

    BoundRelTableInfo(catalog::NodeTableIDPair nodePair, common::RelMultiplicity srcMultiplicity,
        common::RelMultiplicity dstMultiplicity)
        : nodePair{nodePair}, srcMultiplicity{srcMultiplicity}, dstMultiplicity{dstMultiplicity} {}
};

struct BoundExtraCreateRelTableGroupInfo final : BoundExtraCreateTableInfo {
    common::RelMultiplicity srcMultiplicity;
    common::RelMultiplicity dstMultiplicity;
    common::ExtendDirection storageDirection;
    std::vector<BoundRelTableInfo> relTableInfos;
    std::string storage;
    common::StorageFormat storageFormat = common::StorageFormat::NONE;
    // External format name for StorageFormat::EXTERNAL (e.g. "lance"); empty otherwise.
    std::string storageFormatName;
    std::optional<function::TableFunction> scanFunction;
    std::optional<std::shared_ptr<function::TableFuncBindData>> scanBindData;
    std::string foreignDatabaseName;

    explicit BoundExtraCreateRelTableGroupInfo(std::vector<PropertyDefinition> definitions,
        common::RelMultiplicity srcMultiplicity, common::RelMultiplicity dstMultiplicity,
        common::ExtendDirection storageDirection, std::vector<BoundRelTableInfo> relTableInfos,
        std::string storage = "", common::StorageFormat storageFormat = common::StorageFormat::NONE,
        std::optional<function::TableFunction> scanFunction = std::nullopt,
        std::optional<std::shared_ptr<function::TableFuncBindData>> scanBindData = std::nullopt,
        std::string foreignDatabaseName = "", std::string storageFormatName = "")
        : BoundExtraCreateTableInfo{std::move(definitions)}, srcMultiplicity{srcMultiplicity},
          dstMultiplicity{dstMultiplicity}, storageDirection{storageDirection},
          relTableInfos{std::move(relTableInfos)}, storage{std::move(storage)},
          storageFormat{storageFormat}, storageFormatName{std::move(storageFormatName)},
          scanFunction{std::move(scanFunction)}, scanBindData{std::move(scanBindData)},
          foreignDatabaseName{std::move(foreignDatabaseName)} {}

    BoundExtraCreateRelTableGroupInfo(const BoundExtraCreateRelTableGroupInfo& other)
        : BoundExtraCreateTableInfo{copyVector(other.propertyDefinitions)},
          srcMultiplicity{other.srcMultiplicity}, dstMultiplicity{other.dstMultiplicity},
          storageDirection{other.storageDirection}, relTableInfos{other.relTableInfos},
          storage{other.storage}, storageFormat{other.storageFormat},
          storageFormatName{other.storageFormatName}, scanFunction{other.scanFunction},
          scanBindData{other.scanBindData}, foreignDatabaseName{other.foreignDatabaseName} {}

    std::unique_ptr<BoundExtraCreateCatalogEntryInfo> copy() const override {
        return std::make_unique<BoundExtraCreateRelTableGroupInfo>(*this);
    }
};

} // namespace binder
} // namespace lbug

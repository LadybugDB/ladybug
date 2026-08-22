#include "catalog/catalog_entry/node_table_catalog_entry.h"

#include "binder/ddl/bound_create_table_info.h"
#include "common/constants.h"
#include "common/enums/storage_format.h"
#include "common/serializer/deserializer.h"
#include "common/string_utils.h"
#include "storage/storage_version_info.h"
#include <format>

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace catalog {

void SortedByProperty::serialize(common::Serializer& serializer) const {
    serializer.write(propertyName);
    serializer.write(ascending);
}

SortedByProperty SortedByProperty::deserialize(common::Deserializer& deserializer) {
    SortedByProperty result;
    deserializer.deserializeValue(result.propertyName);
    deserializer.deserializeValue(result.ascending);
    return result;
}

static void upgradeLegacyStorageFormat(const std::string& storage,
    common::StorageFormat& storageFormat) {
    const auto lowerStorage = common::StringUtils::getLower(storage);
    if (lowerStorage.ends_with("parquet")) {
        storageFormat = common::StorageFormat::ICEBUG_DISK;
    }
}

void NodeTableCatalogEntry::setPartitionInfo(binder::BoundPartitionMethod method,
    std::string columnName, common::property_id_t columnID, uint64_t numPartitions) {
    partitionMethod = method;
    partitionColumnName = std::move(columnName);
    partitionColumnID = columnID;
    this->numPartitions = numPartitions;
}

std::optional<common::table_id_t> NodeTableCatalogEntry::findListPartition(
    const std::string& encodedKey) const {
    for (const auto& [key, tableID] : listPartitionKeys) {
        if (key == encodedKey) {
            return tableID;
        }
    }
    return std::nullopt;
}

void NodeTableCatalogEntry::addListPartition(std::string encodedKey, common::table_id_t tableID) {
    DASSERT(!findListPartition(encodedKey).has_value());
    listPartitionKeys.emplace_back(std::move(encodedKey), tableID);
}

void NodeTableCatalogEntry::renameProperty(const std::string& propertyName,
    const std::string& newName) {
    TableCatalogEntry::renameProperty(propertyName, newName);
    if (common::StringUtils::caseInsensitiveEquals(propertyName, primaryKeyName)) {
        primaryKeyName = newName;
    }
    for (auto& sortedByProperty : sortedByProperties) {
        if (common::StringUtils::caseInsensitiveEquals(propertyName,
                sortedByProperty.propertyName)) {
            sortedByProperty.propertyName = newName;
        }
    }
}

void NodeTableCatalogEntry::serialize(common::Serializer& serializer) const {
    TableCatalogEntry::serialize(serializer);
    serializer.writeDebuggingInfo("primaryKeyName");
    serializer.write(primaryKeyName);
    serializer.writeDebuggingInfo("sortedByProperties");
    serializer.serializeVector(sortedByProperties);
    serializer.writeDebuggingInfo("csrMetadata");
    serializer.write(csr);
    serializer.write(csrChangeEpoch);
    serializer.writeDebuggingInfo("storage");
    serializer.write(storage);
    serializer.writeDebuggingInfo("storageFormat");
    serializer.serializeValue(storageFormat);
    serializer.writeDebuggingInfo("partitioning");
    serializer.write(partitionMethod.has_value());
    if (partitionMethod.has_value()) {
        serializer.write(static_cast<uint8_t>(*partitionMethod));
        serializer.write(partitionColumnName);
        serializer.write(partitionColumnID);
        serializer.write(numPartitions);
        serializer.serializeVector(childTableIDs);
        // LIST-only: encoded key -> child table id, in creation order. Readers gate on the
        // storage version; writers always emit the current format.
        serializer.write<uint64_t>(listPartitionKeys.size());
        for (const auto& [encodedKey, tableID] : listPartitionKeys) {
            serializer.write(encodedKey);
            serializer.write(tableID);
        }
    }
    serializer.writeDebuggingInfo("partitionChild");
    serializer.write(isPartitionChild());
    if (isPartitionChild()) {
        serializer.write(parentTableID);
        serializer.write(partitionIndex);
    }
}

std::unique_ptr<NodeTableCatalogEntry> NodeTableCatalogEntry::deserialize(
    common::Deserializer& deserializer) {
    std::string debuggingInfo;
    std::string primaryKeyName;
    std::vector<SortedByProperty> sortedByProperties;
    auto csr = false;
    // Must match the uint64_t member and the ALTER record wire type
    uint64_t csrChangeEpoch = 0;
    std::string storage;
    auto storageFormat = StorageFormat::NONE;
    deserializer.validateDebuggingInfo(debuggingInfo, "primaryKeyName");
    deserializer.deserializeValue(primaryKeyName);
    if (deserializer.getStorageVersion() >=
        ::lbug::storage::StorageVersionInfo::STORAGE_VERSION_43) {
        deserializer.validateDebuggingInfo(debuggingInfo, "sortedByProperties");
        deserializer.deserializeVector(sortedByProperties);
    }
    if (deserializer.getStorageVersion() >=
        ::lbug::storage::StorageVersionInfo::STORAGE_VERSION_44) {
        deserializer.validateDebuggingInfo(debuggingInfo, "csrMetadata");
        deserializer.deserializeValue(csr);
        deserializer.deserializeValue(csrChangeEpoch);
    }
    deserializer.validateDebuggingInfo(debuggingInfo, "storage");
    deserializer.deserializeValue(storage);
    if (deserializer.getStorageVersion() >=
        ::lbug::storage::StorageVersionInfo::STORAGE_VERSION_41) {
        deserializer.validateDebuggingInfo(debuggingInfo, "storageFormat");
        deserializer.deserializeValue(storageFormat);
    } else {
        upgradeLegacyStorageFormat(storage, storageFormat);
    }
    auto nodeTableEntry = std::make_unique<NodeTableCatalogEntry>();
    nodeTableEntry->primaryKeyName = primaryKeyName;
    nodeTableEntry->sortedByProperties = std::move(sortedByProperties);
    nodeTableEntry->csr = csr;
    nodeTableEntry->csrChangeEpoch = csrChangeEpoch;
    nodeTableEntry->storage = storage;
    nodeTableEntry->storageFormat = storageFormat;
    if (deserializer.getStorageVersion() >=
        ::lbug::storage::StorageVersionInfo::STORAGE_VERSION_46) {
        deserializer.validateDebuggingInfo(debuggingInfo, "partitioning");
        bool isPartitioned = false;
        deserializer.deserializeValue(isPartitioned);
        if (isPartitioned) {
            uint8_t method = 0;
            std::string columnName;
            auto columnID = common::property_id_t{};
            uint64_t numPartitions = 0;
            std::vector<common::table_id_t> childTableIDs;
            deserializer.deserializeValue(method);
            deserializer.deserializeValue(columnName);
            deserializer.deserializeValue(columnID);
            deserializer.deserializeValue(numPartitions);
            deserializer.deserializeVector(childTableIDs);
            nodeTableEntry->partitionMethod = static_cast<binder::BoundPartitionMethod>(method);
            nodeTableEntry->partitionColumnName = std::move(columnName);
            nodeTableEntry->partitionColumnID = columnID;
            nodeTableEntry->numPartitions = numPartitions;
            nodeTableEntry->childTableIDs = std::move(childTableIDs);
            if (deserializer.getStorageVersion() >=
                ::lbug::storage::StorageVersionInfo::STORAGE_VERSION_47) {
                auto numListKeys = uint64_t{0};
                deserializer.deserializeValue(numListKeys);
                nodeTableEntry->listPartitionKeys.reserve(numListKeys);
                for (auto i = 0u; i < numListKeys; i++) {
                    std::string encodedKey;
                    common::table_id_t tableID;
                    deserializer.deserializeValue(encodedKey);
                    deserializer.deserializeValue(tableID);
                    nodeTableEntry->listPartitionKeys.emplace_back(std::move(encodedKey), tableID);
                }
            }
        }
        deserializer.validateDebuggingInfo(debuggingInfo, "partitionChild");
        bool isPartitionChild = false;
        deserializer.deserializeValue(isPartitionChild);
        if (isPartitionChild) {
            deserializer.deserializeValue(nodeTableEntry->parentTableID);
            deserializer.deserializeValue(nodeTableEntry->partitionIndex);
        }
    }
    return nodeTableEntry;
}

std::string NodeTableCatalogEntry::toCypher(const ToCypherInfo& /*info*/) const {
    auto base = std::format("CREATE NODE TABLE {} ({} PRIMARY KEY({}))",
        common::StringUtils::quoteIdentifier(getName()), propertyCollection.toCypher(),
        common::StringUtils::quoteIdentifier(primaryKeyName));
    if (isPartitioned()) {
        auto method = *partitionMethod == binder::BoundPartitionMethod::HASH ? "HASH" : "RANGE";
        base += std::format(" PARTITION BY {}({}) PARTITIONS {}", method,
            common::StringUtils::quoteIdentifier(partitionColumnName), numPartitions);
    }
    return base + ";";
}

std::unique_ptr<binder::BoundTableScanInfo> NodeTableCatalogEntry::getBoundScanInfo(
    main::ClientContext* context, const std::string& nodeUniqueName) {
    if (scanFunction.has_value()) {
        // Foreign table - call the extension's bind data function
        auto bindData = createBindDataFunc(context, nodeUniqueName);
        return std::make_unique<binder::BoundTableScanInfo>(*scanFunction, std::move(bindData));
    }
    // Check referenced entry (shadow tables: NodeTableCatalogEntry that wraps a foreign entry)
    if (referencedEntry != nullptr) {
        return referencedEntry->getBoundScanInfo(context, nodeUniqueName);
    }
    // Local table - no scan function available
    return nullptr;
}

std::unique_ptr<TableCatalogEntry> NodeTableCatalogEntry::copy() const {
    auto other = std::make_unique<NodeTableCatalogEntry>();
    other->primaryKeyName = primaryKeyName;
    other->sortedByProperties = sortedByProperties;
    other->csr = csr;
    other->csrChangeEpoch = csrChangeEpoch;
    other->storage = storage;
    other->storageFormat = storageFormat;
    other->scanFunction = scanFunction;
    other->createBindDataFunc = createBindDataFunc;
    other->foreignDatabaseName = foreignDatabaseName;
    other->partitionMethod = partitionMethod;
    other->partitionColumnName = partitionColumnName;
    other->partitionColumnID = partitionColumnID;
    other->numPartitions = numPartitions;
    other->childTableIDs = childTableIDs;
    other->parentTableID = parentTableID;
    other->partitionIndex = partitionIndex;
    other->copyFrom(*this);
    return other;
}

std::unique_ptr<BoundExtraCreateCatalogEntryInfo> NodeTableCatalogEntry::getBoundExtraCreateInfo(
    transaction::Transaction*) const {
    std::optional<binder::BoundPartitionInfo> partitionInfo;
    if (isPartitioned()) {
        partitionInfo =
            binder::BoundPartitionInfo(*partitionMethod, partitionColumnName, numPartitions);
    }
    auto boundExtraInfo = std::make_unique<binder::BoundExtraCreateNodeTableInfo>(primaryKeyName,
        copyVector(getProperties()), storage, storageFormat, std::move(partitionInfo));
    if (isPartitionChild()) {
        boundExtraInfo->setPartitionParent(parentTableID, partitionIndex);
    }
    return boundExtraInfo;
}

} // namespace catalog
} // namespace lbug

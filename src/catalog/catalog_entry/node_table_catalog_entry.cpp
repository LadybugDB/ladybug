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

static void upgradeLegacyStorageFormat(const std::string& storage,
    common::StorageFormat& storageFormat) {
    const auto lowerStorage = common::StringUtils::getLower(storage);
    if (lowerStorage.ends_with("parquet")) {
        storageFormat = common::StorageFormat::ICEBUG_DISK;
    }
}

void NodeTableCatalogEntry::renameProperty(const std::string& propertyName,
    const std::string& newName) {
    TableCatalogEntry::renameProperty(propertyName, newName);
    if (common::StringUtils::caseInsensitiveEquals(propertyName, primaryKeyName)) {
        primaryKeyName = newName;
    }
}

void NodeTableCatalogEntry::serialize(common::Serializer& serializer) const {
    TableCatalogEntry::serialize(serializer);
    serializer.writeDebuggingInfo("primaryKeyName");
    serializer.write(primaryKeyName);
    serializer.writeDebuggingInfo("storage");
    serializer.write(storage);
    serializer.writeDebuggingInfo("storageFormat");
    serializer.serializeValue(storageFormat);
}

std::unique_ptr<NodeTableCatalogEntry> NodeTableCatalogEntry::deserialize(
    common::Deserializer& deserializer) {
    std::string debuggingInfo;
    std::string primaryKeyName;
    std::string storage;
    auto storageFormat = StorageFormat::NONE;
    deserializer.validateDebuggingInfo(debuggingInfo, "primaryKeyName");
    deserializer.deserializeValue(primaryKeyName);
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
    nodeTableEntry->storage = storage;
    nodeTableEntry->storageFormat = storageFormat;
    return nodeTableEntry;
}

std::string NodeTableCatalogEntry::toCypher(const ToCypherInfo& /*info*/) const {
    return std::format("CREATE NODE TABLE `{}` ({} PRIMARY KEY(`{}`));", getName(),
        propertyCollection.toCypher(), primaryKeyName);
}

std::optional<function::TableFunction> NodeTableCatalogEntry::getScanFunction() const {
    return scanFunction;
}

std::unique_ptr<binder::BoundTableScanInfo> NodeTableCatalogEntry::getBoundScanInfo(
    main::ClientContext* context, [[maybe_unused]] const std::string& nodeUniqueName) {
    if (scanFunction.has_value()) {
        // Foreign table - call the extension's bind data function
        auto bindData = createBindDataFunc(context);
        return std::make_unique<binder::BoundTableScanInfo>(*scanFunction, std::move(bindData));
    } else {
        // Local table - for now, return nullptr as ForeignNodeTable handles the binding
        return nullptr;
    }
}

std::unique_ptr<TableCatalogEntry> NodeTableCatalogEntry::copy() const {
    auto other = std::make_unique<NodeTableCatalogEntry>();
    other->primaryKeyName = primaryKeyName;
    other->storage = storage;
    other->storageFormat = storageFormat;
    other->scanFunction = scanFunction;
    other->createBindDataFunc = createBindDataFunc;
    other->foreignDatabaseName = foreignDatabaseName;
    other->copyFrom(*this);
    return other;
}

std::unique_ptr<BoundExtraCreateCatalogEntryInfo> NodeTableCatalogEntry::getBoundExtraCreateInfo(
    transaction::Transaction*) const {
    return std::make_unique<BoundExtraCreateNodeTableInfo>(primaryKeyName,
        copyVector(getProperties()), storage, storageFormat);
}

} // namespace catalog
} // namespace lbug

#include "catalog/catalog_entry/node_table_catalog_entry.h"

#include "binder/ddl/bound_create_table_info.h"
#include "common/serializer/deserializer.h"
#include "common/string_utils.h"
#include <format>

using namespace lbug::binder;

namespace lbug {
namespace catalog {

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
    serializer.write(storageFormat);
}

std::unique_ptr<NodeTableCatalogEntry> NodeTableCatalogEntry::deserialize(
    common::Deserializer& deserializer) {
    std::string debuggingInfo;
    std::string primaryKeyName;
    std::string storage;
    std::string storageFormat;
    deserializer.validateDebuggingInfo(debuggingInfo, "primaryKeyName");
    deserializer.deserializeValue(primaryKeyName);
    deserializer.validateDebuggingInfo(debuggingInfo, "storage");
    deserializer.deserializeValue(storage);
    deserializer.validateDebuggingInfo(debuggingInfo, "storageFormat");
    deserializer.deserializeValue(storageFormat);
    auto nodeTableEntry = std::make_unique<NodeTableCatalogEntry>();
    nodeTableEntry->primaryKeyName = primaryKeyName;
    nodeTableEntry->storage = storage;
    nodeTableEntry->storageFormat = storageFormat;
    return nodeTableEntry;
}

std::string NodeTableCatalogEntry::toCypher(const ToCypherInfo& /*info*/) const {
    std::stringstream ss;
    ss << std::format("CREATE NODE TABLE `{}` ({} PRIMARY KEY(`{}`))", getName(),
        propertyCollection.toCypher(), primaryKeyName);

    if (!storage.empty()) {
        ss << std::format(" WITH (STORAGE = '{}'", storage);
        if (!storageFormat.empty()) {
            ss << std::format(", FORMAT = '{}'", storageFormat);
        }
        ss << ")";
    }

    ss << ";";
    return ss.str();
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

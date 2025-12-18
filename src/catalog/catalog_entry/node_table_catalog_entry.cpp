#include "catalog/catalog_entry/node_table_catalog_entry.h"

#include "binder/ddl/bound_create_table_info.h"
#include "common/serializer/deserializer.h"
#include "common/string_utils.h"

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
}

std::unique_ptr<NodeTableCatalogEntry> NodeTableCatalogEntry::deserialize(
    common::Deserializer& deserializer) {
    std::string debuggingInfo;
    std::string primaryKeyName;
    std::string storage;
    deserializer.validateDebuggingInfo(debuggingInfo, "primaryKeyName");
    deserializer.deserializeValue(primaryKeyName);
    deserializer.validateDebuggingInfo(debuggingInfo, "storage");
    deserializer.deserializeValue(storage);
    auto nodeTableEntry = std::make_unique<NodeTableCatalogEntry>();
    nodeTableEntry->primaryKeyName = primaryKeyName;
    nodeTableEntry->storage = storage;
    return nodeTableEntry;
}

std::string NodeTableCatalogEntry::toCypher(const ToCypherInfo& /*info*/) const {
    return common::stringFormat("CREATE NODE TABLE `{}` ({} PRIMARY KEY(`{}`));", getName(),
        propertyCollection.toCypher(), primaryKeyName);
}

function::TableFunction NodeTableCatalogEntry::getScanFunction() {
    if (scanFunction.has_value()) {
        return *scanFunction;
    }
    return TableCatalogEntry::getScanFunction();
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
    other->scanFunction = scanFunction;
    other->createBindDataFunc = createBindDataFunc;
    other->foreignDatabaseName = foreignDatabaseName;
    other->copyFrom(*this);
    return other;
}

std::unique_ptr<BoundExtraCreateCatalogEntryInfo> NodeTableCatalogEntry::getBoundExtraCreateInfo(
    transaction::Transaction*) const {
    return std::make_unique<BoundExtraCreateNodeTableInfo>(primaryKeyName,
        copyVector(getProperties()), storage);
}

} // namespace catalog
} // namespace lbug

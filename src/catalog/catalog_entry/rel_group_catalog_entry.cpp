#include "catalog/catalog_entry/rel_group_catalog_entry.h"

#include <sstream>

#include "binder/ddl/bound_create_table_info.h"
#include "catalog/catalog.h"
#include "common/constants.h"
#include "common/enums/storage_format.h"
#include "common/serializer/deserializer.h"
#include "common/string_utils.h"
#include "storage/storage_version_info.h"
#include "transaction/transaction.h"
#include <format>

using namespace lbug::common;
using namespace lbug::main;

namespace lbug {
namespace catalog {

static void upgradeLegacyStorageFormat(const std::string& storage,
    common::StorageFormat& storageFormat) {
    const auto lowerStorage = common::StringUtils::getLower(storage);
    if (lowerStorage.ends_with("parquet")) {
        storageFormat = common::StorageFormat::ICEBUG_DISK;
    }
}

void RelGroupCatalogEntry::addFromToConnection(table_id_t srcTableID, table_id_t dstTableID,
    oid_t oid) {
    relTableInfos.emplace_back(NodeTableIDPair{srcTableID, dstTableID}, oid, srcMultiplicity,
        dstMultiplicity);
}

void RelGroupCatalogEntry::dropFromToConnection(table_id_t srcTableID, table_id_t dstTableID) {
    auto tmpInfos = relTableInfos;
    relTableInfos.clear();
    for (auto& tmpInfo : tmpInfos) {
        if (tmpInfo.nodePair.srcTableID == srcTableID &&
            tmpInfo.nodePair.dstTableID == dstTableID) {
            continue;
        }
        relTableInfos.emplace_back(tmpInfo);
    }
}

void RelTableCatalogInfo::serialize(Serializer& ser) const {
    ser.writeDebuggingInfo("nodePair");
    nodePair.serialize(ser);
    ser.writeDebuggingInfo("oid");
    ser.serializeValue(oid);
    ser.writeDebuggingInfo("srcMultiplicity");
    ser.serializeValue(srcMultiplicity);
    ser.writeDebuggingInfo("dstMultiplicity");
    ser.serializeValue(dstMultiplicity);
}

RelTableCatalogInfo RelTableCatalogInfo::deserialize(Deserializer& deser) {
    std::string debuggingInfo;
    oid_t oid = INVALID_OID;
    deser.validateDebuggingInfo(debuggingInfo, "nodePair");
    auto nodePair = NodeTableIDPair::deserialize(deser);
    deser.validateDebuggingInfo(debuggingInfo, "oid");
    deser.deserializeValue(oid);
    auto srcMultiplicity = RelMultiplicity::MANY;
    auto dstMultiplicity = RelMultiplicity::MANY;
    if (deser.getStorageVersion() >= ::lbug::storage::StorageVersionInfo::STORAGE_VERSION_42) {
        deser.validateDebuggingInfo(debuggingInfo, "srcMultiplicity");
        deser.deserializeValue(srcMultiplicity);
        deser.validateDebuggingInfo(debuggingInfo, "dstMultiplicity");
        deser.deserializeValue(dstMultiplicity);
    }
    return RelTableCatalogInfo{nodePair, oid, srcMultiplicity, dstMultiplicity};
}

bool RelGroupCatalogEntry::isParent(table_id_t tableID) {
    for (auto& info : relTableInfos) {
        if (info.nodePair.srcTableID == tableID || info.nodePair.dstTableID == tableID) {
            return true;
        }
    }
    return false;
}

const RelTableCatalogInfo& RelGroupCatalogEntry::getSingleRelEntryInfo() const {
    DASSERT(relTableInfos.size() == 1);
    return relTableInfos[0];
}

const RelTableCatalogInfo* RelGroupCatalogEntry::getRelEntryInfo(table_id_t srcTableID,
    table_id_t dstTableID) const {
    for (auto& info : relTableInfos) {
        if (info.nodePair.srcTableID == srcTableID && info.nodePair.dstTableID == dstTableID) {
            return &info;
        }
    }
    return nullptr;
}

std::unordered_set<table_id_t> RelGroupCatalogEntry::getSrcNodeTableIDSet() const {
    std::unordered_set<table_id_t> result;
    for (auto& info : relTableInfos) {
        result.insert(info.nodePair.srcTableID);
    }
    return result;
}

std::unordered_set<table_id_t> RelGroupCatalogEntry::getDstNodeTableIDSet() const {
    std::unordered_set<table_id_t> result;
    for (auto& info : relTableInfos) {
        result.insert(info.nodePair.dstTableID);
    }
    return result;
}

void RelGroupCatalogEntry::serialize(Serializer& serializer) const {
    TableCatalogEntry::serialize(serializer);
    serializer.writeDebuggingInfo("srcMultiplicity");
    serializer.serializeValue(srcMultiplicity);
    serializer.writeDebuggingInfo("dstMultiplicity");
    serializer.serializeValue(dstMultiplicity);
    serializer.writeDebuggingInfo("storageDirection");
    serializer.serializeValue(storageDirection);
    serializer.writeDebuggingInfo("storage");
    serializer.serializeValue(storage);
    serializer.writeDebuggingInfo("scanFunction");
    serializer.serializeValue(scanFunction.has_value());
    if (scanFunction.has_value()) {
        // TODO: serialize TableFunction
    }
    serializer.writeDebuggingInfo("relTableInfos");
    serializer.serializeVector(relTableInfos);
    serializer.writeDebuggingInfo("storageFormat");
    serializer.serializeValue(storageFormat);
}

std::unique_ptr<RelGroupCatalogEntry> RelGroupCatalogEntry::deserialize(
    Deserializer& deserializer) {
    std::string debuggingInfo;
    auto srcMultiplicity = RelMultiplicity::MANY;
    auto dstMultiplicity = RelMultiplicity::MANY;
    auto storageDirection = ExtendDirection::BOTH;
    std::string storage;
    auto storageFormat = StorageFormat::NONE;
    std::vector<RelTableCatalogInfo> relTableInfos;
    deserializer.validateDebuggingInfo(debuggingInfo, "srcMultiplicity");
    deserializer.deserializeValue(srcMultiplicity);
    deserializer.validateDebuggingInfo(debuggingInfo, "dstMultiplicity");
    deserializer.deserializeValue(dstMultiplicity);
    deserializer.validateDebuggingInfo(debuggingInfo, "storageDirection");
    deserializer.deserializeValue(storageDirection);
    deserializer.validateDebuggingInfo(debuggingInfo, "storage");
    deserializer.deserializeValue(storage);
    deserializer.validateDebuggingInfo(debuggingInfo, "scanFunction");
    bool hasScanFunction;
    deserializer.deserializeValue(hasScanFunction);
    std::optional<function::TableFunction> scanFunction = std::nullopt;
    if (hasScanFunction) {
        // TODO: deserialize TableFunction
    }
    deserializer.validateDebuggingInfo(debuggingInfo, "relTableInfos");
    deserializer.deserializeVector(relTableInfos);
    if (deserializer.getStorageVersion() <
        ::lbug::storage::StorageVersionInfo::STORAGE_VERSION_42) {
        for (auto& relTableInfo : relTableInfos) {
            relTableInfo.srcMultiplicity = srcMultiplicity;
            relTableInfo.dstMultiplicity = dstMultiplicity;
        }
    }
    if (deserializer.getStorageVersion() >=
        ::lbug::storage::StorageVersionInfo::STORAGE_VERSION_41) {
        deserializer.validateDebuggingInfo(debuggingInfo, "storageFormat");
        deserializer.deserializeValue(storageFormat);
    } else {
        upgradeLegacyStorageFormat(storage, storageFormat);
    }
    auto relGroupEntry = std::make_unique<RelGroupCatalogEntry>();
    relGroupEntry->srcMultiplicity = srcMultiplicity;
    relGroupEntry->dstMultiplicity = dstMultiplicity;
    relGroupEntry->storageDirection = storageDirection;
    relGroupEntry->storage = storage;
    relGroupEntry->storageFormat = storageFormat;
    relGroupEntry->scanFunction = scanFunction;
    relGroupEntry->relTableInfos = relTableInfos;
    return relGroupEntry;
}

static std::string getFromToStr(const NodeTableIDPair& pair, const Catalog* catalog,
    const transaction::Transaction* transaction, const std::string& storage) {
    std::string srcTableName, dstTableName;
    // For foreign-backed rel tables, node table IDs are FOREIGN_TABLE_ID
    if (pair.srcTableID == common::FOREIGN_TABLE_ID && !storage.empty()) {
        auto dotPos = storage.find('.');
        srcTableName =
            dotPos != std::string::npos ? storage.substr(0, dotPos) + ".nodes" : "foreign_table";
        dstTableName = srcTableName;
    } else {
        srcTableName = catalog->getTableCatalogEntry(transaction, pair.srcTableID)->getName();
        dstTableName = catalog->getTableCatalogEntry(transaction, pair.dstTableID)->getName();
    }
    return std::format("FROM `{}` TO `{}`", srcTableName, dstTableName);
}

static std::string getMultiplicityStr(RelMultiplicity srcMultiplicity,
    RelMultiplicity dstMultiplicity) {
    return RelMultiplicityUtils::toString(srcMultiplicity) + "_" +
           RelMultiplicityUtils::toString(dstMultiplicity);
}

std::string RelGroupCatalogEntry::toCypher(const ToCypherInfo& info) const {
    auto relGroupInfo = info.constCast<RelGroupToCypherInfo>();
    auto catalog = Catalog::Get(*relGroupInfo.context);
    auto transaction = transaction::Transaction::Get(*relGroupInfo.context);
    std::stringstream ss;
    ss << std::format("CREATE REL TABLE `{}` (", getName());
    DASSERT(!relTableInfos.empty());
    ss << getFromToStr(relTableInfos[0].nodePair, catalog, transaction, storage);
    if (relTableInfos[0].srcMultiplicity != srcMultiplicity ||
        relTableInfos[0].dstMultiplicity != dstMultiplicity) {
        ss << " "
           << getMultiplicityStr(relTableInfos[0].srcMultiplicity,
                  relTableInfos[0].dstMultiplicity);
    }
    for (auto i = 1u; i < relTableInfos.size(); ++i) {
        ss << std::format(", {}",
            getFromToStr(relTableInfos[i].nodePair, catalog, transaction, storage));
        if (relTableInfos[i].srcMultiplicity != srcMultiplicity ||
            relTableInfos[i].dstMultiplicity != dstMultiplicity) {
            ss << " "
               << getMultiplicityStr(relTableInfos[i].srcMultiplicity,
                      relTableInfos[i].dstMultiplicity);
        }
    }
    ss << ", " << propertyCollection.toCypher()
       << getMultiplicityStr(srcMultiplicity, dstMultiplicity) << ");";
    return ss.str();
}

std::vector<RelDataDirection> RelGroupCatalogEntry::getRelDataDirections() const {
    switch (storageDirection) {
    case ExtendDirection::FWD: {
        return {RelDataDirection::FWD};
    }
    case ExtendDirection::BWD: {
        return {RelDataDirection::BWD};
    }
    case ExtendDirection::BOTH: {
        return {RelDataDirection::FWD, RelDataDirection::BWD};
    }
    default: {
        UNREACHABLE_CODE;
    }
    }
}

std::unique_ptr<TableCatalogEntry> RelGroupCatalogEntry::copy() const {
    auto other = std::make_unique<RelGroupCatalogEntry>();
    other->srcMultiplicity = srcMultiplicity;
    other->dstMultiplicity = dstMultiplicity;
    other->storageDirection = storageDirection;
    other->storage = storage;
    other->storageFormat = storageFormat;
    other->scanFunction = scanFunction;
    other->scanBindData = std::nullopt; // TODO: implement copy for bindData if needed
    other->foreignDatabaseName = foreignDatabaseName;
    other->relTableInfos = relTableInfos;
    other->copyFrom(*this);
    return other;
}

std::unique_ptr<binder::BoundExtraCreateCatalogEntryInfo>
RelGroupCatalogEntry::getBoundExtraCreateInfo(transaction::Transaction*) const {
    std::vector<binder::BoundRelTableInfo> boundRelTableInfos;
    for (auto& relTableInfo : relTableInfos) {
        boundRelTableInfos.emplace_back(relTableInfo.nodePair, relTableInfo.srcMultiplicity,
            relTableInfo.dstMultiplicity);
    }
    return std::make_unique<binder::BoundExtraCreateRelTableGroupInfo>(
        copyVector(propertyCollection.getDefinitions()), srcMultiplicity, dstMultiplicity,
        storageDirection, std::move(boundRelTableInfos), storage, storageFormat);
}

} // namespace catalog
} // namespace lbug

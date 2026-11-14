#include "catalog/catalog_entry/constraint_catalog_entry.h"

#include "catalog/catalog.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "common/string_utils.h"
#include "transaction/transaction.h"
#include <format>

namespace lbug {
namespace catalog {

std::string ConstraintTypeUtils::toString(ConstraintType type) {
    switch (type) {
    case ConstraintType::UNIQUE:
        return "UNIQUE";
    }
    UNREACHABLE_CODE;
}

bool ConstraintCatalogEntry::containsPropertyID(common::property_id_t propertyID) const {
    for (auto id : propertyIDs) {
        if (id == propertyID) {
            return true;
        }
    }
    return false;
}

void ConstraintCatalogEntry::serialize(common::Serializer& serializer) const {
    CatalogEntry::serialize(serializer);
    serializer.write(tableID);
    serializer.write(tableName);
    serializer.write(isRelConstraint);
    serializer.serializeVector(propertyIDs);
    serializer.serializeVector(propertyNames);
    serializer.write(constraintType);
}

std::unique_ptr<ConstraintCatalogEntry> ConstraintCatalogEntry::deserialize(
    common::Deserializer& deserializer) {
    common::table_id_t tableID = common::INVALID_TABLE_ID;
    std::string tableName;
    bool isRelConstraint = false;
    std::vector<common::property_id_t> propertyIDs;
    std::vector<std::string> propertyNames;
    ConstraintType constraintType = ConstraintType::UNIQUE;
    deserializer.deserializeValue(tableID);
    deserializer.deserializeValue(tableName);
    deserializer.deserializeValue(isRelConstraint);
    deserializer.deserializeVector(propertyIDs);
    deserializer.deserializeVector(propertyNames);
    deserializer.deserializeValue(constraintType);
    // Name is filled in by CatalogEntry::deserialize after this returns.
    return std::make_unique<ConstraintCatalogEntry>("", tableID, std::move(tableName),
        isRelConstraint, std::move(propertyIDs), std::move(propertyNames), constraintType);
}

std::string ConstraintCatalogEntry::toCypher(const ToCypherInfo& info) const {
    // e.g. CREATE CONSTRAINT review_unique FOR ()-[r:REVIEWED]-() REQUIRE (r.userId, r.productId)
    // IS UNIQUE;
    // Resolve names live when a client context is available so RENAME TABLE/PROPERTY is
    // reflected; fall back to the cached creation-time names otherwise.
    std::string liveTableName = tableName;
    std::vector<std::string> livePropertyNames = propertyNames;
    if (auto* cypherInfo = dynamic_cast<const ConstraintToCypherInfo*>(&info);
        cypherInfo != nullptr && cypherInfo->context != nullptr) {
        auto* context = cypherInfo->context;
        auto* catalog = Catalog::Get(*context);
        auto* transaction = transaction::Transaction::Get(*context);
        if (auto* tableEntry = catalog->getTableCatalogEntry(transaction, tableID)) {
            liveTableName = tableEntry->getName();
            livePropertyNames.clear();
            for (auto propertyID : propertyIDs) {
                livePropertyNames.push_back(tableEntry->getProperty(propertyID).getName());
            }
        }
    }
    std::string requireList;
    const std::string var = isRelConstraint ? "r" : "n";
    for (auto i = 0u; i < livePropertyNames.size(); i++) {
        requireList += (i == 0 ? var + "." : ", " + var + ".") + livePropertyNames[i];
    }
    if (isRelConstraint) {
        return std::format("CREATE CONSTRAINT {} FOR ()-[r:{}]-() REQUIRE ({}) IS UNIQUE;",
            getName(), liveTableName, requireList);
    }
    return std::format("CREATE CONSTRAINT {} FOR (n:{}) REQUIRE ({}) IS UNIQUE;", getName(),
        liveTableName, requireList);
}

void ConstraintCatalogEntry::copyFrom(const CatalogEntry& other) {
    CatalogEntry::copyFrom(other);
    auto& otherEntry = other.constCast<ConstraintCatalogEntry>();
    tableID = otherEntry.tableID;
    tableName = otherEntry.tableName;
    isRelConstraint = otherEntry.isRelConstraint;
    propertyIDs = otherEntry.propertyIDs;
    propertyNames = otherEntry.propertyNames;
    constraintType = otherEntry.constraintType;
}

} // namespace catalog
} // namespace lbug

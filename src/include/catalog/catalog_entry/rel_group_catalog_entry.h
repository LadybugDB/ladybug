#pragma once

#include <optional>

#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/enums/extend_direction.h"
#include "common/enums/rel_direction.h"
#include "common/enums/rel_multiplicity.h"
#include "function/table/bind_data.h"
#include "function/table/table_function.h"
#include "node_table_id_pair.h"

namespace lbug {
namespace catalog {

struct RelGroupToCypherInfo final : ToCypherInfo {
    const main::ClientContext* context;

    explicit RelGroupToCypherInfo(const main::ClientContext* context) : context{context} {}
};

struct RelTableCatalogInfo {
    NodeTableIDPair nodePair;
    common::oid_t oid = common::INVALID_OID;

    RelTableCatalogInfo() = default;
    RelTableCatalogInfo(NodeTableIDPair nodePair, common::oid_t oid)
        : nodePair{nodePair}, oid{oid} {}

    void serialize(common::Serializer& ser) const;
    static RelTableCatalogInfo deserialize(common::Deserializer& deser);
};

class LBUG_API RelGroupCatalogEntry final : public TableCatalogEntry {
    static constexpr CatalogEntryType type_ = CatalogEntryType::REL_GROUP_ENTRY;

public:
    RelGroupCatalogEntry() = default;
    RelGroupCatalogEntry(std::string tableName, common::RelMultiplicity srcMultiplicity,
        common::RelMultiplicity dstMultiplicity, common::ExtendDirection storageDirection,
        std::vector<RelTableCatalogInfo> relTableInfos, std::string storage = "",
        std::optional<function::TableFunction> scanFunction = std::nullopt,
        std::optional<std::shared_ptr<function::TableFuncBindData>> scanBindData = std::nullopt,
        std::string foreignDatabaseName = "")
        : TableCatalogEntry{type_, std::move(tableName)}, srcMultiplicity{srcMultiplicity},
          dstMultiplicity{dstMultiplicity}, storageDirection{storageDirection},
          relTableInfos{std::move(relTableInfos)}, storage{std::move(storage)},
          scanFunction{std::move(scanFunction)}, scanBindData{std::move(scanBindData)},
          foreignDatabaseName{std::move(foreignDatabaseName)} {
        propertyCollection =
            PropertyDefinitionCollection{1}; // Skip NBR_NODE_ID column as the first one.
    }

    bool isParent(common::table_id_t tableID) override;
    common::TableType getTableType() const override { return common::TableType::REL; }

    common::RelMultiplicity getMultiplicity(common::RelDataDirection direction) const {
        return direction == common::RelDataDirection::FWD ? dstMultiplicity : srcMultiplicity;
    }
    bool isSingleMultiplicity(common::RelDataDirection direction) const {
        return getMultiplicity(direction) == common::RelMultiplicity::ONE;
    }

    common::ExtendDirection getStorageDirection() const { return storageDirection; }
    const std::string& getStorage() const { return storage; }
    std::optional<function::TableFunction> getScanFunction() const override { return scanFunction; }
    const std::optional<std::shared_ptr<function::TableFuncBindData>>& getScanBindData() const {
        return scanBindData;
    }
    const std::string& getForeignDatabaseName() const { return foreignDatabaseName; }

    common::idx_t getNumRelTables() const { return relTableInfos.size(); }
    const std::vector<RelTableCatalogInfo>& getRelEntryInfos() const { return relTableInfos; }
    const RelTableCatalogInfo& getSingleRelEntryInfo() const;
    bool hasRelEntryInfo(common::table_id_t srcTableID, common::table_id_t dstTableID) const {
        return getRelEntryInfo(srcTableID, dstTableID) != nullptr;
    }
    const RelTableCatalogInfo* getRelEntryInfo(common::table_id_t srcTableID,
        common::table_id_t dstTableID) const;

    std::unordered_set<common::table_id_t> getSrcNodeTableIDSet() const;
    std::unordered_set<common::table_id_t> getDstNodeTableIDSet() const;
    std::unordered_set<common::table_id_t> getBoundNodeTableIDSet(
        common::RelDataDirection direction) const {
        return direction == common::RelDataDirection::FWD ? getSrcNodeTableIDSet() :
                                                            getDstNodeTableIDSet();
    }
    std::unordered_set<common::table_id_t> getNbrNodeTableIDSet(
        common::RelDataDirection direction) const {
        return direction == common::RelDataDirection::FWD ? getDstNodeTableIDSet() :
                                                            getSrcNodeTableIDSet();
    }

    std::vector<common::RelDataDirection> getRelDataDirections() const;

    void addFromToConnection(common::table_id_t srcTableID, common::table_id_t dstTableID,
        common::oid_t oid);
    void dropFromToConnection(common::table_id_t srcTableID, common::table_id_t dstTableID);
    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<RelGroupCatalogEntry> deserialize(common::Deserializer& deserializer);
    std::string toCypher(const ToCypherInfo& info) const override;

    std::unique_ptr<TableCatalogEntry> copy() const override;

protected:
    std::unique_ptr<binder::BoundExtraCreateCatalogEntryInfo> getBoundExtraCreateInfo(
        transaction::Transaction*) const override;

private:
    common::RelMultiplicity srcMultiplicity = common::RelMultiplicity::MANY;
    common::RelMultiplicity dstMultiplicity = common::RelMultiplicity::MANY;
    // TODO(Guodong): Avoid using extend direction for storage direction
    common::ExtendDirection storageDirection = common::ExtendDirection::BOTH;
    std::vector<RelTableCatalogInfo> relTableInfos;
    std::string storage;
    std::optional<function::TableFunction> scanFunction;
    std::optional<std::shared_ptr<function::TableFuncBindData>> scanBindData;
    std::string foreignDatabaseName; // Database name for foreign-backed rel tables
};

} // namespace catalog
} // namespace lbug

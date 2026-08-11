#pragma once

#include <optional>

#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/enums/extend_direction.h"
#include "common/enums/rel_direction.h"
#include "common/enums/rel_multiplicity.h"
#include "common/enums/storage_format.h"
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
    common::RelMultiplicity srcMultiplicity = common::RelMultiplicity::MANY;
    common::RelMultiplicity dstMultiplicity = common::RelMultiplicity::MANY;

    RelTableCatalogInfo() = default;
    RelTableCatalogInfo(NodeTableIDPair nodePair, common::oid_t oid,
        common::RelMultiplicity srcMultiplicity = common::RelMultiplicity::MANY,
        common::RelMultiplicity dstMultiplicity = common::RelMultiplicity::MANY)
        : nodePair{nodePair}, oid{oid}, srcMultiplicity{srcMultiplicity},
          dstMultiplicity{dstMultiplicity} {}

    common::RelMultiplicity getMultiplicity(common::RelDataDirection direction) const {
        return direction == common::RelDataDirection::FWD ? dstMultiplicity : srcMultiplicity;
    }

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
        common::StorageFormat storageFormat = common::StorageFormat::NONE,
        std::optional<function::TableFunction> scanFunction = std::nullopt,
        std::optional<std::shared_ptr<function::TableFuncBindData>> scanBindData = std::nullopt,
        std::string foreignDatabaseName = "")
        : TableCatalogEntry{type_, std::move(tableName)}, srcMultiplicity{srcMultiplicity},
          dstMultiplicity{dstMultiplicity}, storageDirection{storageDirection},
          relTableInfos{std::move(relTableInfos)}, storage{std::move(storage)},
          storageFormat{storageFormat}, scanFunction{std::move(scanFunction)},
          scanBindData{std::move(scanBindData)},
          foreignDatabaseName{std::move(foreignDatabaseName)} {
        propertyCollection =
            PropertyDefinitionCollection{1}; // Skip NBR_NODE_ID column as the first one.
    }

    bool isParent(common::table_id_t tableID) override;
    common::TableType getTableType() const override { return common::TableType::REL; }

    common::RelMultiplicity getMultiplicity(common::RelDataDirection direction) const {
        return direction == common::RelDataDirection::FWD ? dstMultiplicity : srcMultiplicity;
    }
    common::RelMultiplicity getMultiplicity(common::table_id_t srcTableID,
        common::table_id_t dstTableID, common::RelDataDirection direction) const {
        const auto relEntryInfo = getRelEntryInfo(srcTableID, dstTableID);
        DASSERT(relEntryInfo);
        return relEntryInfo->getMultiplicity(direction);
    }
    bool isSingleMultiplicity(common::RelDataDirection direction) const {
        return getMultiplicity(direction) == common::RelMultiplicity::ONE;
    }
    bool isSingleMultiplicity(common::table_id_t srcTableID, common::table_id_t dstTableID,
        common::RelDataDirection direction) const {
        return getMultiplicity(srcTableID, dstTableID, direction) == common::RelMultiplicity::ONE;
    }

    common::ExtendDirection getStorageDirection() const { return storageDirection; }
    const std::string& getStorage() const { return storage; }
    common::StorageFormat getStorageFormat() const { return storageFormat; }

    // CSR sorted-by-dest is an explicit user declaration that the rel
    // table's CSR adjacency lists are physically stored with neighbors
    // (dest node row IDs) in non-decreasing order within each bound
    // (source) row. This lets ArrowQueryResult::CSRArrowArrays::symmetrize()
    // skip the O(m log max-degree) per-row sort and run a two-pointer
    // merge directly against the raw indices. It is a user assertion: the
    // storage engine does not (yet) establish the invariant during
    // checkpoint, so it is the caller's responsibility to only declare it
    // on data that is actually sorted (e.g. loaded from a pre-sorted
    // source). The csrChangeEpoch watermark records the rel table's
    // changeEpoch at declaration time; symmetrize() disregards the flag
    // when the table has been mutated since declaration, falling back to
    // the safe per-row sort path.
    bool isCsrSortedByDest() const { return csrSortedByDest; }
    uint64_t getCsrChangeEpoch() const { return csrChangeEpoch; }
    void setCsrSortedByDest(bool enable, uint64_t changeEpoch) {
        csrSortedByDest = enable;
        csrChangeEpoch = enable ? changeEpoch : 0;
    }
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
    common::StorageFormat storageFormat = common::StorageFormat::NONE;
    std::optional<function::TableFunction> scanFunction;
    std::optional<std::shared_ptr<function::TableFuncBindData>> scanBindData;
    std::string foreignDatabaseName; // Database name for foreign-backed rel tables
    bool csrSortedByDest = false;
    uint64_t csrChangeEpoch = 0;
};

} // namespace catalog
} // namespace lbug

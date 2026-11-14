#pragma once

#include <string>
#include <vector>

#include "catalog_entry.h"
#include "common/types/types.h"

namespace lbug {
namespace main {
class ClientContext;
}
namespace catalog {

// Unique-constraint catalog entry.
//
// A UNIQUE constraint enforces that the tuple of the listed properties is distinct across
// all rows of one node table or one rel group. NULL is treated as distinct (SQL semantics):
// rows with NULL in any constrained property never conflict.
//
// The entry is keyed by constraint name in its own catalog set, and references the target
// table by ID plus property IDs (stable across renames). Property/column type info lives on
// the table entry and is resolved at bind/check time, so ADD/DROP COLUMN keeps working.
//
// Reserved for later: ConstraintType::KEY / NOT_NULL / CHECK share this entry shape
// (expression text + enforced flag) without changing persistence.
enum class ConstraintType : uint8_t {
    UNIQUE = 0,
};

struct ConstraintTypeUtils {
    static std::string toString(ConstraintType type);
};

struct LBUG_API ConstraintToCypherInfo : ToCypherInfo {
    const main::ClientContext* context;
    explicit ConstraintToCypherInfo(const main::ClientContext* context) : context{context} {}
};

class LBUG_API ConstraintCatalogEntry final : public CatalogEntry {
public:
    ConstraintCatalogEntry(std::string constraintName, common::table_id_t tableID,
        std::string tableName, bool isRelConstraint, std::vector<common::property_id_t> propertyIDs,
        std::vector<std::string> propertyNames,
        ConstraintType constraintType = ConstraintType::UNIQUE)
        : CatalogEntry{CatalogEntryType::CONSTRAINT_ENTRY, std::move(constraintName)},
          tableID{tableID}, tableName{std::move(tableName)}, isRelConstraint{isRelConstraint},
          propertyIDs{std::move(propertyIDs)}, propertyNames{std::move(propertyNames)},
          constraintType{constraintType} {}

    std::string getConstraintName() const { return getName(); }
    common::table_id_t getTableID() const { return tableID; }
    // Table IDs shift when WAL replays (see replayCreateCatalogEntryRecord); the recorded
    // ID must be translated through the replayed-ID map like INDEX_ENTRY does.
    void setTableID(common::table_id_t tableID_) { tableID = tableID_; }
    std::string getTableName() const { return tableName; }
    void setTableName(std::string name) { tableName = std::move(name); }
    bool isRel() const { return isRelConstraint; }
    ConstraintType getConstraintType() const { return constraintType; }

    const std::vector<common::property_id_t>& getPropertyIDs() const { return propertyIDs; }
    const std::vector<std::string>& getPropertyNames() const { return propertyNames; }
    bool containsPropertyID(common::property_id_t propertyID) const;

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<ConstraintCatalogEntry> deserialize(common::Deserializer& deserializer);

    std::string toCypher(const ToCypherInfo& info) const override;

    void copyFrom(const CatalogEntry& other) override;

    std::unique_ptr<ConstraintCatalogEntry> copy() const {
        return std::make_unique<ConstraintCatalogEntry>(getName(), tableID, tableName,
            isRelConstraint, propertyIDs, propertyNames, constraintType);
    }

private:
    common::table_id_t tableID = common::INVALID_TABLE_ID;
    std::string tableName;
    bool isRelConstraint = false;
    std::vector<common::property_id_t> propertyIDs;
    std::vector<std::string> propertyNames;
    ConstraintType constraintType = ConstraintType::UNIQUE;
};

} // namespace catalog
} // namespace lbug

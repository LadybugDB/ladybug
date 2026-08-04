#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/enums/conflict_action.h"
#include "common/enums/table_type.h"
#include "parsed_property_definition.h"

namespace lbug {
namespace parser {

struct ExtraCreateTableInfo {
    virtual ~ExtraCreateTableInfo() = default;

    template<class TARGET>
    const TARGET& constCast() const {
        return common::dynamic_cast_checked<const TARGET&>(*this);
    }
};

// PostgreSQL-style partitioning method applied to a node table.
enum class ParsedPartitionMethod : uint8_t { HASH = 0, RANGE = 1 };

struct ParsedPartitionInfo {
    ParsedPartitionMethod method;
    std::string columnName;
    uint64_t numPartitions;

    ParsedPartitionInfo(ParsedPartitionMethod method, std::string columnName,
        uint64_t numPartitions)
        : method{method}, columnName{std::move(columnName)}, numPartitions{numPartitions} {}
};

struct CreateTableInfo {
    common::TableType type;
    std::string tableName;
    std::vector<ParsedPropertyDefinition> propertyDefinitions;
    std::unique_ptr<ExtraCreateTableInfo> extraInfo;
    common::ConflictAction onConflict;

    CreateTableInfo(common::TableType type, std::string tableName,
        common::ConflictAction onConflict)
        : type{type}, tableName{std::move(tableName)}, extraInfo{nullptr}, onConflict{onConflict} {}
    DELETE_COPY_DEFAULT_MOVE(CreateTableInfo);
};

struct ExtraCreateNodeTableInfo final : ExtraCreateTableInfo {
    std::string pKName;
    options_t options;
    std::optional<ParsedPartitionInfo> partitionInfo;

    explicit ExtraCreateNodeTableInfo(std::string pKName, options_t options = {},
        std::optional<ParsedPartitionInfo> partitionInfo = std::nullopt)
        : pKName{std::move(pKName)}, options{std::move(options)},
          partitionInfo{std::move(partitionInfo)} {}
};

struct ParsedRelConnection {
    std::string srcTableName;
    std::string dstTableName;
    std::optional<std::string> relMultiplicity;

    ParsedRelConnection(std::string srcTableName, std::string dstTableName,
        std::optional<std::string> relMultiplicity)
        : srcTableName{std::move(srcTableName)}, dstTableName{std::move(dstTableName)},
          relMultiplicity{std::move(relMultiplicity)} {}
};

struct ExtraCreateRelTableGroupInfo final : ExtraCreateTableInfo {
    std::string relMultiplicity;
    std::vector<ParsedRelConnection> connections;
    options_t options;

    ExtraCreateRelTableGroupInfo(std::string relMultiplicity,
        std::vector<ParsedRelConnection> connections, options_t options)
        : relMultiplicity{std::move(relMultiplicity)}, connections{std::move(connections)},
          options{std::move(options)} {}
};

} // namespace parser
} // namespace lbug

#pragma once

#include <string>
#include <vector>

#include "common/enums/conflict_action.h"
#include "parser/statement.h"

namespace lbug {
namespace parser {

// Parsed info for:
//   CREATE CONSTRAINT <name> [IF NOT EXISTS]
//   FOR (n:Label) | FOR ()-[r:RELTYPE]-()
//   REQUIRE (r.prop [, ...]) IS UNIQUE   (single-prop form without parens also allowed)
struct CreateConstraintInfo {
    std::string constraintName;
    // Pattern variable (n or r) and the table (node label or rel type) it binds to.
    std::string variableName;
    std::string tableName;
    bool isRelConstraint = false;
    std::vector<std::string> propertyNames;
    common::ConflictAction onConflict;

    CreateConstraintInfo(std::string constraintName, std::string variableName,
        std::string tableName, bool isRelConstraint, std::vector<std::string> propertyNames,
        common::ConflictAction onConflict)
        : constraintName{std::move(constraintName)}, variableName{std::move(variableName)},
          tableName{std::move(tableName)}, isRelConstraint{isRelConstraint},
          propertyNames{std::move(propertyNames)}, onConflict{onConflict} {}
};

class CreateConstraint final : public Statement {
    static constexpr common::StatementType type_ = common::StatementType::CREATE_CONSTRAINT;

public:
    explicit CreateConstraint(CreateConstraintInfo info)
        : Statement{type_}, info{std::move(info)} {}

    const CreateConstraintInfo& getInfo() const { return info; }

private:
    CreateConstraintInfo info;
};

} // namespace parser
} // namespace lbug

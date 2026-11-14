#pragma once

#include <string>

#include "common/enums/conflict_action.h"
#include "parser/statement.h"

namespace lbug {
namespace parser {

// Parsed info for: DROP CONSTRAINT <name> [IF EXISTS]
struct DropConstraintInfo {
    std::string constraintName;
    common::ConflictAction onConflict;

    DropConstraintInfo(std::string constraintName, common::ConflictAction onConflict)
        : constraintName{std::move(constraintName)}, onConflict{onConflict} {}
};

class DropConstraint final : public Statement {
    static constexpr common::StatementType type_ = common::StatementType::DROP_CONSTRAINT;

public:
    explicit DropConstraint(DropConstraintInfo info) : Statement{type_}, info{std::move(info)} {}

    const DropConstraintInfo& getInfo() const { return info; }

private:
    DropConstraintInfo info;
};

} // namespace parser
} // namespace lbug

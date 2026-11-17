#pragma once

#include <string>

#include "binder/bound_statement.h"
#include "common/enums/conflict_action.h"

namespace lbug {
namespace binder {

struct BoundDropConstraintInfo {
    std::string constraintName;
    common::ConflictAction onConflict;
};

class BoundDropConstraint final : public BoundStatement {
    static constexpr common::StatementType type_ = common::StatementType::DROP_CONSTRAINT;

public:
    explicit BoundDropConstraint(BoundDropConstraintInfo info)
        : BoundStatement{type_, BoundStatementResult::createSingleStringColumnResult()},
          info{std::move(info)} {}

    const BoundDropConstraintInfo& getInfo() const { return info; }

private:
    BoundDropConstraintInfo info;
};

} // namespace binder
} // namespace lbug

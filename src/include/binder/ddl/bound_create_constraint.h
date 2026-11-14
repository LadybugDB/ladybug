#pragma once

#include <string>
#include <vector>

#include "binder/bound_statement.h"
#include "common/enums/conflict_action.h"
#include "common/types/types.h"

namespace lbug {
namespace binder {

struct BoundCreateConstraintInfo {
    std::string constraintName;
    std::string tableName;
    common::table_id_t tableID;
    bool isRelConstraint;
    std::vector<std::string> propertyNames;
    std::vector<common::property_id_t> propertyIDs;
    common::ConflictAction onConflict;

    BoundCreateConstraintInfo copy() const { return *this; }

    std::string toString() const;
};

class BoundCreateConstraint final : public BoundStatement {
    static constexpr common::StatementType type_ = common::StatementType::CREATE_CONSTRAINT;

public:
    explicit BoundCreateConstraint(BoundCreateConstraintInfo info)
        : BoundStatement{type_, BoundStatementResult::createSingleStringColumnResult()},
          info{std::move(info)} {}

    const BoundCreateConstraintInfo& getInfo() const { return info; }

private:
    BoundCreateConstraintInfo info;
};

} // namespace binder
} // namespace lbug

#pragma once

#include "binder/bound_statement.h"
#include "common/enums/conflict_action.h"
#include "common/types/types.h"

namespace lbug {
namespace binder {

struct BoundCreateIndexInfo {
    std::string indexType;
    std::string indexName;
    std::string tableName;
    common::table_id_t tableID;
    std::string propertyName;
    common::property_id_t propertyID;
    common::column_id_t columnID;
    common::PhysicalTypeID keyDataType;
    bool isPrimary;
    common::ConflictAction onConflict;

    BoundCreateIndexInfo copy() const { return *this; }

    std::string toString() const;
};

class BoundCreateIndex final : public BoundStatement {
    static constexpr common::StatementType type_ = common::StatementType::CREATE_INDEX;

public:
    explicit BoundCreateIndex(BoundCreateIndexInfo info)
        : BoundStatement{type_, BoundStatementResult::createSingleStringColumnResult()},
          info{std::move(info)} {}

    const BoundCreateIndexInfo& getInfo() const { return info; }

private:
    BoundCreateIndexInfo info;
};

} // namespace binder
} // namespace lbug

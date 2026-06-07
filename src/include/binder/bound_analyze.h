#pragma once

#include <optional>

#include "bound_statement.h"

namespace lbug {
namespace binder {

class BoundAnalyze final : public BoundStatement {
    static constexpr common::StatementType statementType_ = common::StatementType::ANALYZE;

public:
    explicit BoundAnalyze(std::optional<std::string> tableName)
        : BoundStatement{statementType_, BoundStatementResult::createSingleStringColumnResult()},
          tableName{std::move(tableName)} {}

    const std::optional<std::string>& getTableName() const { return tableName; }

private:
    std::optional<std::string> tableName;
};

} // namespace binder
} // namespace lbug

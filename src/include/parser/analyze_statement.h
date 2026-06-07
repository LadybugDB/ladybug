#pragma once

#include <optional>

#include "statement.h"

namespace lbug {
namespace parser {

class AnalyzeStatement final : public Statement {
    static constexpr common::StatementType statementType_ = common::StatementType::ANALYZE;

public:
    explicit AnalyzeStatement(std::optional<std::string> tableName)
        : Statement{statementType_}, tableName{std::move(tableName)} {}

    const std::optional<std::string>& getTableName() const { return tableName; }

private:
    std::optional<std::string> tableName;
};

} // namespace parser
} // namespace lbug

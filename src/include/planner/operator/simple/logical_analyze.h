#pragma once

#include <optional>

#include "planner/operator/logical_operator.h"

namespace lbug {
namespace planner {

class LogicalAnalyze final : public LogicalOperator {
    static constexpr LogicalOperatorType type_ = LogicalOperatorType::ANALYZE;

public:
    explicit LogicalAnalyze(std::optional<std::string> tableName)
        : LogicalOperator{type_}, tableName{std::move(tableName)} {}

    const std::optional<std::string>& getTableName() const { return tableName; }

    std::string getExpressionsForPrinting() const final { return tableName.value_or("ALL"); }

    void computeFlatSchema() final { createEmptySchema(); }
    void computeFactorizedSchema() final { createEmptySchema(); }

    std::unique_ptr<LogicalOperator> copy() final {
        return std::make_unique<LogicalAnalyze>(tableName);
    }

private:
    std::optional<std::string> tableName;
};

} // namespace planner
} // namespace lbug

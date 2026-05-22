#pragma once

#include "planner/operator/logical_operator.h"

namespace lbug {
namespace planner {

class LogicalUnwindDeduplicate final : public LogicalOperator {
    static constexpr LogicalOperatorType type_ = LogicalOperatorType::UNWIND_DEDUPLICATE;

public:
    LogicalUnwindDeduplicate(std::shared_ptr<LogicalOperator> child,
        binder::expression_vector keyExpressions)
        : LogicalOperator{type_, std::move(child)}, keyExpressions{std::move(keyExpressions)} {}

    void computeFactorizedSchema() override;
    void computeFlatSchema() override { copyChildSchema(0); }

    std::string getExpressionsForPrinting() const override { return {}; }

    const binder::expression_vector& getKeyExpressions() const { return keyExpressions; }

    std::unique_ptr<LogicalOperator> copy() override {
        return std::make_unique<LogicalUnwindDeduplicate>(children[0]->copy(), keyExpressions);
    }

private:
    binder::expression_vector keyExpressions;
};

} // namespace planner
} // namespace lbug

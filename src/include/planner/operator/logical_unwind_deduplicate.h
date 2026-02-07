#pragma once

#include "planner/operator/logical_operator.h"

namespace lbug {
namespace planner {

class LogicalUnwindDeduplicate final : public LogicalOperator {
    static constexpr LogicalOperatorType type_ = LogicalOperatorType::UNWIND_DEDUPLICATE;

public:
    LogicalUnwindDeduplicate(std::shared_ptr<LogicalOperator> child,
        std::shared_ptr<binder::Expression> keyExpression)
        : LogicalOperator{type_, std::move(child)}, keyExpression{std::move(keyExpression)} {}

    void computeFactorizedSchema() override;
    void computeFlatSchema() override { copyChildSchema(0); }

    std::string getExpressionsForPrinting() const override { return {}; }

    std::shared_ptr<binder::Expression> getKeyExpression() const { return keyExpression; }

    std::unique_ptr<LogicalOperator> copy() override {
        return make_unique<LogicalUnwindDeduplicate>(children[0]->copy(), keyExpression);
    }

private:
    std::shared_ptr<binder::Expression> keyExpression;
};

} // namespace planner
} // namespace lbug

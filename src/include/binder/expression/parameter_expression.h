#pragma once

#include <memory>

#include "common/types/value/value.h"
#include "expression.h"

namespace lbug {
namespace binder {

class LBUG_API ParameterExpression final : public Expression {
    static constexpr common::ExpressionType expressionType = common::ExpressionType::PARAMETER;

public:
    explicit ParameterExpression(const std::string& parameterName, common::Value value)
        : Expression{expressionType, value.getDataType().copy(), createUniqueName(parameterName)},
          parameterName(parameterName), value{std::make_shared<common::Value>(std::move(value))} {}

    explicit ParameterExpression(const std::string& parameterName,
        std::shared_ptr<common::Value> value)
        : Expression{expressionType, value->getDataType().copy(), createUniqueName(parameterName)},
          parameterName(parameterName), value{std::move(value)} {}

    void cast(const common::LogicalType& type) override;

    common::Value getValue() const { return *value; }

    // Records that this parameter's value was frozen into a plan at plan-build time
    // (bind/optimize/map), e.g. via ExpressionUtil::evaluateAsSkipLimit. Such statements
    // cannot reuse a cached physical plan when the parameter value changes, so
    // ClientContext keeps them off the plan cache (see
    // https://github.com/LadybugDB/ladybug/issues/985).
    //
    // Protocol for future code: if you bake a parameter value into a plan (anything other
    // than re-reading the shared Value at execution time, cf.
    // LiteralExpressionEvaluator::resolveResultVector), route the read through a helper
    // that marks the parameter (like evaluateAsSkipLimit does). The mark is monotonic and
    // lives on the bound expression object, so any later plan-time read of the same object
    // stays marked. Mutable so plan-time helpers taking `const Expression&` can mark.
    void markBakedIntoPlan() const { bakedIntoPlan = true; }
    bool wasBakedIntoPlan() const { return bakedIntoPlan; }

private:
    std::string toStringInternal() const override { return "$" + parameterName; }
    static std::string createUniqueName(const std::string& input) { return "$" + input; }

private:
    std::string parameterName;
    std::shared_ptr<common::Value> value;
    mutable bool bakedIntoPlan = false;
};

} // namespace binder
} // namespace lbug

#pragma once

#include "expression.h"
#include "function/aggregate_function.h"

namespace lbug {
namespace binder {

class AggregateFunctionExpression final : public Expression {
    static constexpr common::ExpressionType expressionType_ =
        common::ExpressionType::AGGREGATE_FUNCTION;

public:
    AggregateFunctionExpression(function::AggregateFunction function,
        std::unique_ptr<function::FunctionBindData> bindData, expression_vector children,
        std::string uniqueName)
        : Expression{expressionType_, bindData->resultType.copy(), std::move(children),
              std::move(uniqueName)},
          function{std::move(function)}, bindData{std::move(bindData)} {}

    const function::AggregateFunction& getFunction() const { return function; }
    function::FunctionBindData* getBindData() const { return bindData.get(); }
    bool isDistinct() const { return function.isDistinct; }
    // Aggregates whose bindFunc derives the result type from their children (e.g.
    // PERCENTILEDISC) can be ANY-typed while those children are still unresolved ANY
    // placeholders for parameters with unknown values. Such statements are always re-bound with
    // concrete parameter types before execution, so resolving the provisional ANY here (as the
    // DefaultTypeSolver does for every other ANY-typed projection expression) is safe. Casts
    // away from a concrete type remain rejected as before.
    void cast(const common::LogicalType& type) override;

    std::string toStringInternal() const override;

    static std::string getUniqueName(const std::string& functionName,
        const expression_vector& children, bool isDistinct);

private:
    function::AggregateFunction function;
    std::unique_ptr<function::FunctionBindData> bindData;
};

} // namespace binder
} // namespace lbug

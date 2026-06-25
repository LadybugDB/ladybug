#pragma once

#include "logical_operator.h"

namespace lbug {
namespace planner {

class LogicalUnion : public LogicalOperator {
public:
    LogicalUnion(binder::expression_vector expressions,
        const std::vector<std::shared_ptr<LogicalOperator>>& children)
        : LogicalOperator{LogicalOperatorType::UNION_ALL, children},
          expressionsToUnion{std::move(expressions)} {}

    void setChildProjections(std::vector<binder::expression_vector> projections) {
        childProjections = std::move(projections);
    }

    f_group_pos_set getGroupsPosToFlatten(uint32_t childIdx);

    void computeFactorizedSchema() override;
    void computeFlatSchema() override;

    std::string getExpressionsForPrinting() const override { return std::string{}; }

    binder::expression_vector getExpressionsToUnion() const { return expressionsToUnion; }

    const std::vector<binder::expression_vector>& getChildProjections() const {
        return childProjections;
    }

    Schema* getSchemaBeforeUnion(uint32_t idx) const { return children[idx]->getSchema(); }

    std::unique_ptr<LogicalOperator> copy() override;

private:
    // If an expression to union has different flat/unflat state in different child, we
    // need to flatten that expression in all the single queries.
    bool requireFlatExpression(uint32_t expressionIdx);

private:
    binder::expression_vector expressionsToUnion;
    // Non-deduplicated per-child projection lists, indexed by child ordinal then column.
    // This preserves the positional correspondence with expressionsToUnion even when a
    // child projects the same expression more than once (which the schema's
    // expressionsInScope deduplicates).
    std::vector<binder::expression_vector> childProjections;
};

} // namespace planner
} // namespace lbug

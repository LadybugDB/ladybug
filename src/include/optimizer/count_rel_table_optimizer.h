#pragma once

#include "logical_operator_visitor.h"
#include "planner/operator/logical_plan.h"

namespace lbug {
namespace main {
class ClientContext;
}

namespace optimizer {

/**
 * This optimizer detects patterns where we're counting all rows from a single rel table
 * without any filters, and replaces the scan + aggregate with a direct count from table metadata.
 *
 * Pattern detected:
 *   AGGREGATE (COUNT_STAR only, no keys) →
 *   PROJECTION (empty or pass-through) →
 *   EXTEND (single rel table) →
 *   SCAN_NODE_TABLE
 *
 * This pattern is replaced with:
 *   COUNT_REL_TABLE (new operator that directly reads the count from table metadata)
 */
class CountRelTableOptimizer : public LogicalOperatorVisitor {
public:
    explicit CountRelTableOptimizer(main::ClientContext* context) : context{context} {}

    void rewrite(planner::LogicalPlan* plan);

private:
    std::shared_ptr<planner::LogicalOperator> visitOperator(
        const std::shared_ptr<planner::LogicalOperator>& op);

    std::shared_ptr<planner::LogicalOperator> visitAggregateReplace(
        std::shared_ptr<planner::LogicalOperator> op) override;

    // Check if the aggregate is a simple COUNT(*) with no keys
    bool isSimpleCountStar(planner::LogicalOperator* op) const;

    // Check if the plan below aggregate matches the pattern for optimization
    bool canOptimize(planner::LogicalOperator* aggregate) const;

    main::ClientContext* context;
};

} // namespace optimizer
} // namespace lbug

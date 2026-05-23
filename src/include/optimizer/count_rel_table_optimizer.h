#pragma once

#include "logical_operator_visitor.h"
#include "planner/operator/logical_plan.h"

namespace lbug {
namespace binder {
class Expression;
class NodeExpression;
class RelExpression;
} // namespace binder
namespace main {
class ClientContext;
}

namespace optimizer {

/**
 * This optimizer detects patterns where we're counting all rows from a single rel table
 * without any filters, and replaces the scan + aggregate with a direct count from table metadata.
 *
 * Pattern detected:
 *   AGGREGATE (COUNT_STAR or COUNT(rel), no keys) →
 *   PROJECTION (empty or pass-through) →
 *   EXTEND (single rel table) →
 *   SCAN_NODE_TABLE
 *
 * This pattern is replaced with:
 *   COUNT_REL_TABLE (new operator that directly reads the count from table metadata)
 */
class CountRelTableOptimizer : public LogicalOperatorVisitor {
public:
    explicit CountRelTableOptimizer(main::ClientContext* context) : _context{context} {}

    void rewrite(planner::LogicalPlan* plan);

private:
    std::shared_ptr<planner::LogicalOperator> visitOperator(
        const std::shared_ptr<planner::LogicalOperator>& op);

    std::shared_ptr<planner::LogicalOperator> visitAggregateReplace(
        std::shared_ptr<planner::LogicalOperator> op) override;
    std::shared_ptr<planner::LogicalOperator> visitOrderByReplace(
        std::shared_ptr<planner::LogicalOperator> op) override;

    // Check if the aggregate is a simple COUNT(*) or COUNT(expr) with no keys.
    bool isSimpleCount(planner::LogicalOperator* op) const;

    bool isCountStar(planner::LogicalOperator* op) const;
    bool isCountRelID(planner::LogicalOperator* op, const binder::RelExpression& rel) const;
    bool isDistinctCountNodeKey(planner::LogicalOperator* op,
        const std::shared_ptr<binder::Expression>& nodeKey) const;
    bool isCountNbr(planner::LogicalOperator* op, const binder::NodeExpression& nbr) const;
    bool isRelIDExpression(const std::shared_ptr<binder::Expression>& expression,
        const binder::RelExpression& rel) const;

    // Check if the plan below aggregate matches the pattern for optimization
    bool canOptimize(planner::LogicalOperator* aggregate) const;
    std::shared_ptr<planner::LogicalOperator> tryRewriteActiveBoundCount(
        std::shared_ptr<planner::LogicalOperator> op);
    std::shared_ptr<planner::LogicalOperator> tryRewriteDegreeTopK(
        std::shared_ptr<planner::LogicalOperator> op);

    [[maybe_unused]] main::ClientContext* _context;
};

} // namespace optimizer
} // namespace lbug

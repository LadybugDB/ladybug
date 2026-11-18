#pragma once

#include "logical_operator_visitor.h"
#include "planner/operator/logical_plan.h"

namespace lbug {
namespace main {
class ClientContext;
}
namespace planner {
class LogicalAggregate;
} // namespace planner

namespace optimizer {

/**
 * This optimizer stops a keyless COUNT(*) from enumerating matches that differ only in columns
 * nothing above reads. When the build side of an inner node-ID hash join outputs only its join
 * keys, each probe row matches as many build rows as the key has, so the build side is replaced
 * with one row per key carrying that number. The COUNT(*) becomes a sum of the products of these
 * multiplicities.
 *
 * Pattern detected:
 *   AGGREGATE (COUNT_STAR, no keys) →
 *   tree of PROJECTION, FILTER, EXTEND, SCAN_NODE_TABLE and inner node-ID HASH_JOIN, with some
 *   HASH_JOIN whose build side is a PROJECTION of exactly its join keys over at least one EXTEND
 *
 * Each such build side is replaced with:
 *   AGGREGATE (join keys, COUNT_STAR or SUM of the multiplicities below it) → build side
 */
class EagerCountOptimizer : public LogicalOperatorVisitor {
public:
    explicit EagerCountOptimizer(main::ClientContext* context) : context{context} {}

    void rewrite(planner::LogicalPlan* plan);

private:
    std::shared_ptr<planner::LogicalOperator> visitOperator(
        const std::shared_ptr<planner::LogicalOperator>& op);

    std::shared_ptr<planner::LogicalOperator> visitAggregateReplace(
        std::shared_ptr<planner::LogicalOperator> op) override;

    // Adds the multiplicities of the pre-aggregated build sides under op to op's output and
    // returns them.
    binder::expression_vector preAggregateBuildSides(planner::LogicalOperator* op);

    // The product of multiplicities, as INT128 so that it cannot overflow before the final cast.
    std::shared_ptr<binder::Expression> bindProduct(const binder::expression_vector& factors);
    // COUNT(*) over child when there are no multiplicities below it, otherwise their summed
    // product, grouped by keys.
    std::shared_ptr<planner::LogicalAggregate> aggregateMultiplicities(
        binder::expression_vector keys, const binder::expression_vector& multiplicities,
        std::shared_ptr<planner::LogicalOperator> child);
    std::string getMultiplicityName();

private:
    main::ClientContext* context;
    uint64_t numMultiplicities = 0;
};

} // namespace optimizer
} // namespace lbug

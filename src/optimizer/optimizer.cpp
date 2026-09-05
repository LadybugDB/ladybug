#include "optimizer/optimizer.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#include "common/enums/extend_direction_util.h"
#include "main/client_context.h"
#include "optimizer/acc_hash_join_optimizer.h"
#include "optimizer/agg_key_dependency_optimizer.h"
#include "optimizer/bool_folding_optimizer.h"
#include "optimizer/cardinality_updater.h"
#include "optimizer/correlated_subquery_unnest_solver.h"
#include "optimizer/count_rel_table_optimizer.h"
#include "optimizer/factorization_rewriter.h"
#include "optimizer/filter_push_down_optimizer.h"
#include "optimizer/foreign_join_push_down_optimizer.h"
#include "optimizer/group_key_predicate_push_down_optimizer.h"
#include "optimizer/limit_push_down_optimizer.h"
#include "optimizer/order_by_push_down_optimizer.h"
#include "optimizer/projection_push_down_optimizer.h"
#include "optimizer/remove_factorization_rewriter.h"
#include "optimizer/remove_unnecessary_distinct_optimizer.h"
#include "optimizer/remove_unnecessary_join_optimizer.h"
#include "optimizer/remove_unnecessary_order_by_optimizer.h"
#include "optimizer/schema_populator.h"
#include "optimizer/top_k_optimizer.h"
#include "optimizer/unwind_dedup_optimizer.h"
#include "planner/operator/extend/logical_extend.h"
#include "planner/operator/logical_aggregate.h"
#include "planner/operator/logical_explain.h"
#include "planner/operator/logical_filter.h"
#include "planner/operator/logical_hash_join.h"
#include "planner/operator/logical_operator.h"
#include "planner/operator/scan/logical_scan_node_table.h"
#include "transaction/transaction.h"

namespace lbug {
namespace optimizer {

namespace {

// Prints one operator per line as an indented tree. Enabled by setting LBUG_DUMP_LOGICAL in the
// environment; unlike EXPLAIN LOGICAL this needs no query changes and shows the plan exactly as
// the optimizer sees it, before and after optimization.
void dumpLogicalTree(const planner::LogicalOperator* op, int depth,
    std::unordered_set<const planner::LogicalOperator*>& visited) {
    if (depth > 40 || visited.contains(op)) {
        for (auto i = 0; i < depth; ++i) {
            fprintf(stderr, "  ");
        }
        fprintf(stderr, "...\n");
        return;
    }
    visited.insert(op);
    for (auto i = 0; i < depth; ++i) {
        fprintf(stderr, "  ");
    }
    fprintf(stderr, "%s",
        planner::LogicalOperatorUtils::logicalOperatorTypeToString(op->getOperatorType()).c_str());
    if (op->getOperatorType() == planner::LogicalOperatorType::EXTEND ||
        op->getOperatorType() == planner::LogicalOperatorType::PACKED_EXTEND) {
        auto& ext = op->constCast<planner::LogicalExtend>();
        fprintf(stderr, " [%s %s bound=%s nbr=%s]", ext.getRel()->detailsToString().c_str(),
            common::ExtendDirectionUtil::toString(ext.getDirection()).c_str(),
            ext.getBoundNode()->getUniqueName().c_str(), ext.getNbrNode()->getUniqueName().c_str());
    } else if (op->getOperatorType() == planner::LogicalOperatorType::FILTER) {
        fprintf(stderr, " [%s]",
            op->constCast<planner::LogicalFilter>().getPredicate()->toString().c_str());
    } else if (op->getOperatorType() == planner::LogicalOperatorType::HASH_JOIN) {
        auto& join = op->constCast<planner::LogicalHashJoin>();
        auto jt = join.getJoinType() == common::JoinType::INNER ? "INNER" :
                  join.getJoinType() == common::JoinType::MARK  ? "MARK" :
                  join.getJoinType() == common::JoinType::LEFT  ? "LEFT" :
                                                                  "COUNT";
        fprintf(stderr, " [%s keys=%s", jt,
            join.getJoinNodeIDs().size() == 1 ? join.getJoinNodeIDs()[0]->toString().c_str() :
                                                "...");
        if (join.hasMark()) {
            fprintf(stderr, " mark=%s]", join.getMark()->toString().c_str());
        } else {
            fprintf(stderr, "]");
        }
    } else if (op->getOperatorType() == planner::LogicalOperatorType::AGGREGATE) {
        fprintf(stderr, " [keys=%llu aggs=%s]",
            (unsigned long long)op->constCast<planner::LogicalAggregate>().getKeys().size(),
            op->constCast<planner::LogicalAggregate>().getAggregates()[0]->toString().c_str());
    } else if (op->getOperatorType() == planner::LogicalOperatorType::SCAN_NODE_TABLE) {
        fprintf(stderr, " [%s]",
            op->constCast<planner::LogicalScanNodeTable>().getNodeID()->toString().c_str());
    }
    fprintf(stderr, "\n");
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        dumpLogicalTree(op->getChild(i).get(), depth + 1, visited);
    }
}

void dumpLogicalPlan(const planner::LogicalPlan* plan, const char* label) {
    fprintf(stderr, "=== LOGICAL PLAN (%s) ===\n", label);
    auto* root = plan->getLastOperator().get();
    if (root == nullptr) {
        return;
    }
    std::unordered_set<const planner::LogicalOperator*> visited;
    dumpLogicalTree(root, 0, visited);
    fprintf(stderr, "=== END LOGICAL PLAN ===\n");
}

} // namespace

void Optimizer::optimize(planner::LogicalPlan* plan, main::ClientContext* context,
    const planner::CardinalityEstimator& cardinalityEstimator) {
    static const bool dumpLogicalEnabled = getenv("LBUG_DUMP_LOGICAL") != nullptr;
    if (dumpLogicalEnabled) {
        dumpLogicalPlan(plan, "before optimization");
    }
    if (context->getClientConfig()->enablePlanOptimizer) {
        // Factorization structure should be removed before further optimization can be applied.
        auto removeFactorizationRewriter = RemoveFactorizationRewriter();
        removeFactorizationRewriter.rewrite(plan);

        auto correlatedSubqueryUnnestSolver = CorrelatedSubqueryUnnestSolver(nullptr);
        correlatedSubqueryUnnestSolver.solve(plan->getLastOperator().get());

        auto removeUnnecessaryJoinOptimizer = RemoveUnnecessaryJoinOptimizer();
        removeUnnecessaryJoinOptimizer.rewrite(plan);

        // UnwindDedupOptimizer should be applied after factorization is removed
        // to avoid issues with computeFlatSchema.
        auto unwindDedupOptimizer = UnwindDedupOptimizer();
        unwindDedupOptimizer.rewrite(plan);

        // CountRelTableOptimizer should be applied early before other optimizations
        // that might change the plan structure.
        auto countRelTableOptimizer = CountRelTableOptimizer(context);
        countRelTableOptimizer.rewrite(plan);

        // ForeignJoinPushDownOptimizer should run before filter push down to detect
        // the full pattern before it gets modified.
        auto foreignJoinPushDownOptimizer = ForeignJoinPushDownOptimizer(context);
        foreignJoinPushDownOptimizer.rewrite(plan);

        // Boolean folding should run before filter push-down so that
        // contradictory or always-true predicates are simplified first.
        auto boolFoldingOptimizer = BoolFoldingOptimizer();
        boolFoldingOptimizer.rewrite(plan);

        // A WITH ... WHERE predicate that depends only on aggregate grouping keys can be
        // evaluated before aggregation. Moving it first allows regular filter push-down to turn
        // otherwise quadratic cross products into joins.
        auto groupKeyPredicatePushDownOptimizer = GroupKeyPredicatePushDownOptimizer();
        groupKeyPredicatePushDownOptimizer.rewrite(plan);

        auto filterPushDownOptimizer = FilterPushDownOptimizer(context, &cardinalityEstimator);
        filterPushDownOptimizer.rewrite(plan);

        auto projectionPushDownOptimizer =
            ProjectionPushDownOptimizer(context->getClientConfig()->recursivePatternSemantic);
        projectionPushDownOptimizer.rewrite(plan);

        auto orderByPushDownOptimizer = OrderByPushDownOptimizer();
        orderByPushDownOptimizer.rewrite(plan);

        auto limitPushDownOptimizer = LimitPushDownOptimizer();
        limitPushDownOptimizer.rewrite(plan);

        if (context->getClientConfig()->enableSemiMask) {
            // HashJoinSIPOptimizer should be applied after optimizers that manipulate hash join.
            auto hashJoinSIPOptimizer = HashJoinSIPOptimizer();
            hashJoinSIPOptimizer.rewrite(plan);
        }

        auto topKOptimizer = TopKOptimizer();
        topKOptimizer.rewrite(plan);

        // Degree top-k rewrites need the LIMIT to be folded into ORDER_BY first.
        countRelTableOptimizer.rewrite(plan);

        auto factorizationRewriter = FactorizationRewriter();
        factorizationRewriter.rewrite(plan);

        // AggKeyDependencyOptimizer doesn't change factorization structure and thus can be put
        // after FactorizationRewriter.
        auto aggKeyDependencyOptimizer = AggKeyDependencyOptimizer();
        aggKeyDependencyOptimizer.rewrite(plan);

        // RemoveUnnecessaryDistinctOptimizer should run after AggKeyDependencyOptimizer
        // so that dependent-key resolution has already happened.
        auto removeUnnecessaryDistinctOptimizer = RemoveUnnecessaryDistinctOptimizer();
        removeUnnecessaryDistinctOptimizer.rewrite(plan);

        // RemoveUnnecessaryOrderByOptimizer removes ORDER BY nodes that do not
        // affect the result (e.g., before a GROUP BY-less aggregate like COUNT(*)).
        auto removeUnnecessaryOrderByOptimizer = RemoveUnnecessaryOrderByOptimizer();
        removeUnnecessaryOrderByOptimizer.rewrite(plan);

        // for EXPLAIN LOGICAL we need to update the cardinalities for the optimized plan
        // we don't need to do this otherwise as we don't use the cardinalities after planning
        if (plan->getLastOperatorRef().getOperatorType() == planner::LogicalOperatorType::EXPLAIN) {
            const auto& explain = plan->getLastOperatorRef().cast<planner::LogicalExplain>();
            if (explain.getExplainType() == common::ExplainType::LOGICAL_PLAN) {
                auto cardinalityUpdater = CardinalityUpdater(cardinalityEstimator,
                    transaction::Transaction::Get(*context));
                cardinalityUpdater.rewrite(plan);
            }
        }
    } else {
        // we still need to compute the schema for each operator even if we have optimizations
        // disabled
        auto schemaPopulator = SchemaPopulator{};
        schemaPopulator.rewrite(plan);
    }
    if (dumpLogicalEnabled) {
        dumpLogicalPlan(plan, "after optimization");
    }
}

} // namespace optimizer
} // namespace lbug

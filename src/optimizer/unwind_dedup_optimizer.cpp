#include "optimizer/unwind_dedup_optimizer.h"

#include "planner/operator/logical_hash_join.h"
#include "planner/operator/logical_unwind.h"
#include "planner/operator/logical_unwind_deduplicate.h"
#include "planner/operator/persistent/logical_merge.h"

using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace optimizer {

void UnwindDedupOptimizer::rewrite(LogicalPlan* plan) {
    visitOperator(plan->getLastOperator());
}

std::shared_ptr<LogicalOperator> UnwindDedupOptimizer::visitOperator(
    const std::shared_ptr<LogicalOperator>& op) {
    // bottom-up traversal
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        op->setChild(i, visitOperator(op->getChild(i)));
    }
    auto result = visitOperatorReplaceSwitch(op);
    result->computeFlatSchema();
    return result;
}

std::shared_ptr<LogicalOperator> UnwindDedupOptimizer::visitMergeReplace(
    std::shared_ptr<LogicalOperator> op) {
    auto merge = op->ptrCast<LogicalMerge>();
    if (merge == nullptr) {
        return op;
    }
    auto mergeChild = merge->getChild(0);
    if (mergeChild->getOperatorType() != LogicalOperatorType::HASH_JOIN) {
        return op;
    }
    auto hashJoin = mergeChild->ptrCast<LogicalHashJoin>();
    auto probeChild = hashJoin->getChild(0);
    if (probeChild->getOperatorType() != LogicalOperatorType::UNWIND) {
        return op;
    }
    auto unwind = probeChild->ptrCast<LogicalUnwind>();
    auto dedup = std::make_shared<LogicalUnwindDeduplicate>(probeChild, unwind->getOutExpr());
    dedup->computeFlatSchema();
    hashJoin->setChild(0, dedup);
    return op;
}

} // namespace optimizer
} // namespace lbug

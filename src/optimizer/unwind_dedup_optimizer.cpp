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

// Helper function to recursively find UNWIND in the operator tree
static std::shared_ptr<LogicalUnwind> findUnwind(std::shared_ptr<LogicalOperator> op) {
    if (op->getOperatorType() == LogicalOperatorType::UNWIND) {
        return std::static_pointer_cast<LogicalUnwind>(op);
    }
    // Recursively search through children
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        auto result = findUnwind(op->getChild(i));
        if (result != nullptr) {
            return result;
        }
    }
    return nullptr;
}

std::shared_ptr<LogicalOperator> UnwindDedupOptimizer::visitMergeReplace(
    std::shared_ptr<LogicalOperator> op) {
    auto merge = op->ptrCast<LogicalMerge>();
    if (merge == nullptr) {
        return op;
    }

    // Check if MERGE has ON MATCH or ON CREATE clauses
    // If it does, we should NOT apply UNWIND_DEDUP because duplicates need different handling:
    // - First occurrence: ON CREATE
    // - Subsequent occurrences: ON MATCH
    bool hasOnMatchOrOnCreate =
        !merge->getOnMatchSetNodeInfos().empty() || !merge->getOnMatchSetRelInfos().empty() ||
        !merge->getOnCreateSetNodeInfos().empty() || !merge->getOnCreateSetRelInfos().empty();
    if (hasOnMatchOrOnCreate) {
        return op;
    }

    auto mergeChild = merge->getChild(0);
    if (mergeChild->getOperatorType() != LogicalOperatorType::HASH_JOIN) {
        return op;
    }
    auto hashJoin = mergeChild->ptrCast<LogicalHashJoin>();
    auto probeChild = hashJoin->getChild(0);

    // Try to find UNWIND either as direct child or nested deeper
    std::shared_ptr<LogicalUnwind> unwind;
    if (probeChild->getOperatorType() == LogicalOperatorType::UNWIND) {
        // Direct UNWIND child (original case)
        unwind = std::static_pointer_cast<LogicalUnwind>(probeChild);
    } else {
        // Search recursively for UNWIND (handles FLATTEN and other intermediate operators)
        unwind = findUnwind(probeChild);
    }

    if (unwind != nullptr) {
        // Wrap the probe child with UNWIND_DEDUP
        auto dedup = std::make_shared<LogicalUnwindDeduplicate>(probeChild, unwind->getOutExpr());
        dedup->computeFlatSchema();
        hashJoin->setChild(0, dedup);
        return op;
    }

    return op;
}

} // namespace optimizer
} // namespace lbug

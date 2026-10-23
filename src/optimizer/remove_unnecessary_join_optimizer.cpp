#include "optimizer/remove_unnecessary_join_optimizer.h"

#include "planner/operator/logical_accumulate.h"
#include "planner/operator/logical_cross_product.h"
#include "planner/operator/logical_hash_join.h"
#include "planner/operator/scan/logical_scan_node_table.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace optimizer {

void RemoveUnnecessaryJoinOptimizer::rewrite(LogicalPlan* plan) {
    visitOperator(plan->getLastOperator());
}

std::shared_ptr<LogicalOperator> RemoveUnnecessaryJoinOptimizer::visitOperator(
    const std::shared_ptr<LogicalOperator>& op) {
    // bottom-up traversal
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        op->setChild(i, visitOperator(op->getChild(i)));
    }
    auto result = visitOperatorReplaceSwitch(op);
    result->computeFlatSchema();
    return result;
}

// No default labels in these two: -Wswitch flags a new type, which counts as NULL-producing
// until classified.
static bool nullsBuildOnlyColumns(JoinType joinType) {
    switch (joinType) {
    case JoinType::INNER:
        return false;
    case JoinType::MARK:
    case JoinType::COUNT:
        // These output only their probe side, so no build column reaches the parent.
        return false;
    case JoinType::LEFT:
        return true;
    }
    return true;
}

static bool isOptional(AccumulateType accumulateType) {
    switch (accumulateType) {
    case AccumulateType::REGULAR:
        return false;
    case AccumulateType::OPTIONAL_:
        return true;
    }
    return true;
}

// An OPTIONAL MATCH that finds nothing binds its variables to NULL. They come from the build
// side of a LEFT join or an optional cross product, or from under an optional accumulate.
// MERGE's existence check also plans an OPTIONAL MATCH, marked; MERGE creates what it does
// not find, so those leave no NULLs.
static bool canBeNull(const LogicalOperator& op, const Expression& key) {
    switch (op.getOperatorType()) {
    case LogicalOperatorType::HASH_JOIN: {
        const auto& join = op.constCast<LogicalHashJoin>();
        if (nullsBuildOnlyColumns(join.getJoinType()) && !join.hasMark() &&
            !op.getChild(0)->getSchema()->isExpressionInScope(key) &&
            op.getChild(1)->getSchema()->isExpressionInScope(key)) {
            return true;
        }
    } break;
    case LogicalOperatorType::CROSS_PRODUCT: {
        const auto& crossProduct = op.constCast<LogicalCrossProduct>();
        if (isOptional(crossProduct.getAccumulateType()) && !crossProduct.hasMark() &&
            op.getChild(1)->getSchema()->isExpressionInScope(key)) {
            return true;
        }
    } break;
    case LogicalOperatorType::ACCUMULATE: {
        const auto& accumulate = op.constCast<LogicalAccumulate>();
        if (isOptional(accumulate.getAccumulateType()) && !accumulate.hasMark() &&
            op.getChild(0)->getSchema()->isExpressionInScope(key)) {
            return true;
        }
    } break;
    default:
        // Other operators pass a key through or bind it non-NULL (scan, extend, MERGE).
        break;
    }
    for (auto i = 0u; i < op.getNumChildren(); ++i) {
        if (canBeNull(*op.getChild(i), key)) {
            return true;
        }
    }
    return false;
}

// Joining with a full node scan drops the rows whose key is NULL, so the join is only
// redundant when the kept side's keys are never NULL.
static bool keptSideKeysCanBeNull(const LogicalHashJoin& hashJoin, uint32_t keptChildIdx) {
    for (auto& [probeKey, buildKey] : hashJoin.getJoinConditions()) {
        const auto& key = keptChildIdx == 0 ? *probeKey : *buildKey;
        if (canBeNull(*hashJoin.getChild(keptChildIdx), key)) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<LogicalOperator> RemoveUnnecessaryJoinOptimizer::visitHashJoinReplace(
    std::shared_ptr<LogicalOperator> op) {
    auto hashJoin = (LogicalHashJoin*)op.get();
    switch (hashJoin->getJoinType()) {
    case JoinType::MARK:
    case JoinType::LEFT: {
        // Do not prune no-trivial join type
        return op;
    }
    default:
        break;
    }
    // TODO(Xiyang): Double check on these changes here.
    // NOTE: CountRelTableOptimizer::tryRewriteGroupedReachableCount relies on this pruning
    // staying limited to bare scans. When the source join of a variable-length path is gone,
    // that rewrite treats it as proof the source is an unfiltered full-table scan (see
    // matchFullTableRecursiveSide). Pruning through a filter, a predicate-carrying scan or
    // any other row-narrowing operator would silently produce wrong aggregate results --
    // keep this condition tight.
    if (op->getChild(1)->getOperatorType() == LogicalOperatorType::SCAN_NODE_TABLE) {
        const auto scanNode = dynamic_cast_checked<LogicalScanNodeTable*>(op->getChild(1).get());
        if (scanNode->getProperties().empty() && !keptSideKeysCanBeNull(*hashJoin, 0)) {
            // Build side is trivial. Prune build side.
            return op->getChild(0);
        }
    }
    if (op->getChild(0)->getOperatorType() == LogicalOperatorType::SCAN_NODE_TABLE) {
        const auto scanNode = dynamic_cast_checked<LogicalScanNodeTable*>(op->getChild(0).get());
        if (scanNode->getProperties().empty() && !keptSideKeysCanBeNull(*hashJoin, 1)) {
            // Probe side is trivial. Prune probe side.
            return op->getChild(1);
        }
    }
    return op;
}

} // namespace optimizer
} // namespace lbug

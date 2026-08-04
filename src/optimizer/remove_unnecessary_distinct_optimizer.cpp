#include "optimizer/remove_unnecessary_distinct_optimizer.h"

#include "binder/expression/property_expression.h"
#include "planner/operator/logical_distinct.h"
#include "planner/operator/logical_operator.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace optimizer {

void RemoveUnnecessaryDistinctOptimizer::rewrite(LogicalPlan* plan) {
    plan->setLastOperator(visitOperator(plan->getLastOperator()));
}

std::shared_ptr<planner::LogicalOperator> RemoveUnnecessaryDistinctOptimizer::visitOperator(
    std::shared_ptr<planner::LogicalOperator> op) {
    // bottom-up traversal: rewrite children first
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        op->setChild(i, visitOperator(op->getChild(i)));
    }
    auto result = visitOperatorReplaceSwitch(op);
    // This optimizer runs after FactorizationRewriter, so the plan is in
    // factorized form. Recompute the factorized schema (computeFlatSchema would
    // be incorrect here and would also throw on FLATTEN nodes).
    result->computeFactorizedSchema();
    return result;
}

std::shared_ptr<planner::LogicalOperator>
RemoveUnnecessaryDistinctOptimizer::visitOperatorReplaceSwitch(
    std::shared_ptr<planner::LogicalOperator> op) {
    if (op->getOperatorType() != LogicalOperatorType::DISTINCT) {
        return op;
    }
    auto& distinct = op->cast<LogicalDistinct>();
    if (isRedundantDistinct(distinct)) {
        // Bypass the DISTINCT: return its (already-rewritten) child. The caller
        // recomputes the factorized schema for the parent, so the parent picks
        // up the child's schema cleanly.
        return op->getChild(0);
    }
    return op;
}

bool RemoveUnnecessaryDistinctOptimizer::isRedundantDistinct(
    const planner::LogicalDistinct& distinct) {
    const auto& keys = distinct.getKeys();
    if (keys.empty()) {
        // DISTINCT with no explicit keys deduplicates on every column. We can't
        // prove that's a no-op, so keep it.
        return false;
    }
    // (a) Every key must be a property that is a primary key or the internal ID.
    // A single unique key only guarantees uniqueness of the *entire* row when the
    // input has multiplicity 1 (checked below); but if any key is not itself
    // unique (e.g. a plain property), DISTINCT can still collapse rows, so we
    // require ALL keys to be uniqueness-guaranteeing.
    for (auto& key : keys) {
        if (key->expressionType != ExpressionType::PROPERTY) {
            return false;
        }
        auto& property = key->constCast<PropertyExpression>();
        if (!property.isPrimaryKey() && !property.isInternalID()) {
            return false;
        }
    }
    // (b) The input must provably have multiplicity 1 on the keys. We only trust
    // a linear chain of non-multiplying passthrough operators ending at a plain
    // node scan. Anything else (joins, extends, recursive/path scans, materialized
    // subplans read via EXPRESSIONS_SCAN/TABLE_FUNCTION_CALL, UNWIND, ...) can
    // duplicate rows, so DISTINCT must be kept.
    return isChildMultiplicityOne(distinct.getChild(0));
}

bool RemoveUnnecessaryDistinctOptimizer::isChildMultiplicityOne(
    std::shared_ptr<planner::LogicalOperator> op) {
    auto cur = op;
    while (cur != nullptr) {
        switch (cur->getOperatorType()) {
        // Safe leaves: a plain node scan produces each node exactly once.
        case LogicalOperatorType::SCAN_NODE_TABLE:
        case LogicalOperatorType::INDEX_LOOK_UP:
            return true;
        // Non-multiplying passthrough operators (single child only).
        case LogicalOperatorType::PROJECTION:
        case LogicalOperatorType::FILTER:
        case LogicalOperatorType::MULTIPLICITY_REDUCER:
        case LogicalOperatorType::LIMIT:
        case LogicalOperatorType::FLATTEN:
        case LogicalOperatorType::NODE_LABEL_FILTER:
        case LogicalOperatorType::NOOP:
            if (cur->getNumChildren() != 1) {
                return false;
            }
            cur = cur->getChild(0);
            break;
        // Anything else (joins, extends, recursive/path scans, set operations,
        // UNWIND, materialized-subplan reads, ...) can duplicate rows.
        default:
            return false;
        }
    }
    return false;
}

} // namespace optimizer
} // namespace lbug

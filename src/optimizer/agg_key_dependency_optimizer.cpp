#include "optimizer/agg_key_dependency_optimizer.h"

#include "binder/expression/aggregate_function_expression.h"
#include "binder/expression/expression_util.h"
#include "binder/expression/property_expression.h"
#include "function/aggregate_function.h"
#include "planner/operator/logical_aggregate.h"
#include "planner/operator/logical_distinct.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace optimizer {

void AggKeyDependencyOptimizer::rewrite(planner::LogicalPlan* plan) {
    visitOperator(plan->getLastOperator().get());
}

void AggKeyDependencyOptimizer::visitOperator(planner::LogicalOperator* op) {
    // bottom up traversal
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        visitOperator(op->getChild(i).get());
    }
    visitOperatorSwitch(op);
}

void AggKeyDependencyOptimizer::visitAggregate(planner::LogicalOperator* op) {
    auto agg = (LogicalAggregate*)op;
    auto [keys, dependentKeys] = resolveKeysAndDependentKeys(agg->getKeys());
    agg->setKeys(keys);
    agg->setDependentKeys(dependentKeys);
    // A COLLECT built per group key set K0 yields exactly one list per K0 group, so the
    // collected variable is functionally determined by K0. Record that for downstream
    // DISTINCT/GROUP BY key reduction below. Binder unique names distinguish rebinding.
    for (auto& aggExpr : agg->getAggregates()) {
        if (aggExpr->expressionType != ExpressionType::AGGREGATE_FUNCTION) {
            continue;
        }
        auto& aggFunc = aggExpr->constCast<AggregateFunctionExpression>();
        if (aggFunc.getFunction().name != function::CollectFunction::name ||
            aggFunc.getNumChildren() != 1) {
            continue;
        }
        std::unordered_set<std::string> groupNames;
        for (auto& key : agg->getKeys()) {
            groupNames.insert(key->getUniqueName());
        }
        collectVarDeps[aggExpr->getUniqueName()] = std::move(groupNames);
    }
}

void AggKeyDependencyOptimizer::visitDistinct(planner::LogicalOperator* op) {
    auto distinct = (LogicalDistinct*)op;
    auto [keys, dependentKeys] = resolveKeysAndDependentKeys(distinct->getKeys());
    distinct->setKeys(keys);
    distinct->setPayloads(dependentKeys);
}

std::pair<binder::expression_vector, binder::expression_vector>
AggKeyDependencyOptimizer::resolveKeysAndDependentKeys(const expression_vector& inputKeys) {
    // Consider example RETURN a.ID, a.age, COUNT(*).
    // We first collect a.ID into primaryKeys. Then collect "a" into primaryVarNames.
    // Finally, we loop through all group by keys to put non-primary key properties under name "a"
    // into dependentKeyExpressions.

    // Collect primary variables from keys.
    std::unordered_set<std::string> primaryVarNames;
    for (auto& key : inputKeys) {
        if (key->expressionType == ExpressionType::PROPERTY) {
            auto property = (PropertyExpression*)key.get();
            if (property->isPrimaryKey() || property->isInternalID()) {
                primaryVarNames.insert(property->getVariableName());
            }
        }
    }
    // Collect input key names for collect-built dependency checks below.
    std::unordered_set<std::string> inputKeyNames;
    for (auto& key : inputKeys) {
        inputKeyNames.insert(key->getUniqueName());
    }
    // Resolve key dependency.
    binder::expression_vector keys;
    binder::expression_vector dependentKeys;
    for (auto& key : inputKeys) {
        // A COLLECT-built list is functionally determined by the group keys it was built
        // over. If those are all present here, rows agreeing on the remaining keys
        // agree on the list too, so it rides as payload instead of being hashed.
        auto collectIt = collectVarDeps.find(key->getUniqueName());
        // An empty group-key set would match vacuously; a global (keyless) collect is a
        // single group and must stay a key (dropping it collapses grouping and can
        // crash on empty key sets).
        if (collectIt != collectVarDeps.end() && !collectIt->second.empty()) {
            bool determined = true;
            for (auto& groupName : collectIt->second) {
                if (!inputKeyNames.contains(groupName)) {
                    determined = false;
                    break;
                }
            }
            if (determined) {
                dependentKeys.push_back(key);
                continue;
            }
        }
        if (key->expressionType == ExpressionType::PROPERTY) {
            auto property = (PropertyExpression*)key.get();
            if (property->isPrimaryKey() ||
                property->isInternalID()) { // NOLINT(bugprone-branch-clone): Collapsing
                                            // is a logical error.
                // Primary properties are always keys.
                keys.push_back(key);
            } else if (primaryVarNames.contains(property->getVariableName())) {
                // Properties depend on any primary property are dependent keys.
                // e.g. a.age depends on a._id
                dependentKeys.push_back(key);
            } else {
                keys.push_back(key);
            }
        } else if (ExpressionUtil::isNodePattern(*key) || ExpressionUtil::isRelPattern(*key)) {
            if (primaryVarNames.contains(key->getUniqueName())) {
                // e.g. a depends on a._id
                dependentKeys.push_back(key);
            } else {
                keys.push_back(key);
            }
        } else {
            keys.push_back(key);
        }
    }
    return std::make_pair(std::move(keys), std::move(dependentKeys));
}

} // namespace optimizer
} // namespace lbug

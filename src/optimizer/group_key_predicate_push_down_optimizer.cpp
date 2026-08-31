#include "optimizer/group_key_predicate_push_down_optimizer.h"

#include "binder/expression_visitor.h"
#include "common/enums/accumulate_type.h"
#include "common/enums/join_type.h"
#include "planner/operator/logical_accumulate.h"
#include "planner/operator/logical_aggregate.h"
#include "planner/operator/logical_filter.h"
#include "planner/operator/logical_hash_join.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace optimizer {

void GroupKeyPredicatePushDownOptimizer::rewrite(LogicalPlan* plan) {
    plan->setLastOperator(visitOperator(plan->getLastOperator()));
}

std::shared_ptr<LogicalOperator> GroupKeyPredicatePushDownOptimizer::visitOperator(
    std::shared_ptr<LogicalOperator> op) {
    // Rewrite bottom-up so a filter sees the final shape of its aggregate input.
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        op->setChild(i, visitOperator(op->getChild(i)));
    }
    op->computeFlatSchema();
    if (op->getOperatorType() == LogicalOperatorType::FILTER) {
        return tryRewriteFilter(std::move(op));
    }
    return op;
}

std::shared_ptr<LogicalOperator> GroupKeyPredicatePushDownOptimizer::tryRewriteFilter(
    std::shared_ptr<LogicalOperator> op) {
    auto& filter = op->cast<LogicalFilter>();
    auto aggregateOp = findAggregateThroughProjections(filter.getChild(0));
    if (aggregateOp == nullptr) {
        return op;
    }
    auto& aggregate = aggregateOp->cast<LogicalAggregate>();
    if (!aggregate.hasKeys()) {
        // A keyless aggregate always emits one row, even for empty input. Moving a predicate below
        // it could therefore change an empty result into a single aggregate row.
        return op;
    }

    std::unordered_set<std::string> groupKeyNames;
    for (auto& key : aggregate.getAllKeys()) {
        groupKeyNames.insert(key->getUniqueName());
    }

    expression_vector predicatesToPush;
    expression_vector predicatesToKeep;
    for (auto& predicate : filter.getPredicate()->splitOnAND()) {
        if (isGroupKeyOnlyPredicate(*predicate, groupKeyNames)) {
            predicatesToPush.push_back(predicate);
        } else {
            predicatesToKeep.push_back(predicate);
        }
    }
    if (predicatesToPush.empty()) {
        return op;
    }

    auto aggregateChild = aggregate.getChild(0);
    for (auto& predicate : predicatesToPush) {
        aggregateChild = pushPredicate(std::move(aggregateChild), predicate);
    }
    aggregate.setChild(0, std::move(aggregateChild));

    // The moved filters preserve every operator's output schema, but recomputing keeps schema
    // ownership and group positions consistent after rewiring.
    recomputeFlatSchemas(filter.getChild(0));

    auto result = filter.getChild(0);
    for (auto& predicate : predicatesToKeep) {
        result = appendFilter(std::move(result), predicate, filter.getCardinality());
    }
    return result;
}

std::shared_ptr<LogicalOperator>
GroupKeyPredicatePushDownOptimizer::findAggregateThroughProjections(
    const std::shared_ptr<LogicalOperator>& op) {
    auto current = op;
    while (current->getOperatorType() == LogicalOperatorType::PROJECTION) {
        if (current->getNumChildren() != 1) {
            return nullptr;
        }
        current = current->getChild(0);
    }
    return current->getOperatorType() == LogicalOperatorType::AGGREGATE ? current : nullptr;
}

bool GroupKeyPredicatePushDownOptimizer::isGroupKeyOnlyPredicate(const Expression& predicate,
    const std::unordered_set<std::string>& groupKeyNames) {
    return !containsRandomFunction(predicate) && isComposedFromGroupKeys(predicate, groupKeyNames);
}

bool GroupKeyPredicatePushDownOptimizer::isComposedFromGroupKeys(const Expression& expression,
    const std::unordered_set<std::string>& groupKeyNames) {
    // An expression projected by an earlier query part may itself be an aggregate expression, but
    // it is an ordinary grouping key in the current aggregate. Check key identity before checking
    // the expression kind for that reason.
    if (groupKeyNames.contains(expression.getUniqueName())) {
        return true;
    }
    switch (expression.expressionType) {
    case ExpressionType::LITERAL:
    case ExpressionType::PARAMETER:
        return true;
    case ExpressionType::AGGREGATE_FUNCTION:
    case ExpressionType::SUBQUERY:
        return false;
    default:
        break;
    }
    auto children = ExpressionChildrenCollector::collectChildren(expression);
    if (children.empty()) {
        return false;
    }
    for (auto& child : children) {
        if (!isComposedFromGroupKeys(*child, groupKeyNames)) {
            return false;
        }
    }
    return true;
}

bool GroupKeyPredicatePushDownOptimizer::containsRandomFunction(const Expression& expression) {
    if (ExpressionVisitor::isRandom(expression)) {
        return true;
    }
    for (auto& child : ExpressionChildrenCollector::collectChildren(expression)) {
        if (containsRandomFunction(*child)) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<LogicalOperator> GroupKeyPredicatePushDownOptimizer::pushPredicate(
    std::shared_ptr<LogicalOperator> op, const std::shared_ptr<Expression>& predicate) {
    switch (op->getOperatorType()) {
    case LogicalOperatorType::PROJECTION:
    case LogicalOperatorType::FILTER:
    case LogicalOperatorType::NODE_LABEL_FILTER: {
        if (op->getNumChildren() == 1 && op->getChild(0)->getSchema()->evaluable(*predicate)) {
            op->setChild(0, pushPredicate(op->getChild(0), predicate));
            op->computeFlatSchema();
            return op;
        }
    } break;
    case LogicalOperatorType::ACCUMULATE: {
        auto& accumulate = op->cast<LogicalAccumulate>();
        if (accumulate.getAccumulateType() == AccumulateType::REGULAR && !accumulate.hasMark() &&
            op->getChild(0)->getSchema()->evaluable(*predicate)) {
            op->setChild(0, pushPredicate(op->getChild(0), predicate));
            op->computeFlatSchema();
            return op;
        }
    } break;
    case LogicalOperatorType::HASH_JOIN: {
        auto& join = op->cast<LogicalHashJoin>();
        auto inProbe = op->getChild(0)->getSchema()->evaluable(*predicate);
        auto inBuild = op->getChild(1)->getSchema()->evaluable(*predicate);
        if (inProbe && !inBuild &&
            (join.getJoinType() == JoinType::INNER || join.getJoinType() == JoinType::LEFT)) {
            op->setChild(0, pushPredicate(op->getChild(0), predicate));
            op->computeFlatSchema();
            return op;
        }
        if (!inProbe && inBuild && join.getJoinType() == JoinType::INNER) {
            op->setChild(1, pushPredicate(op->getChild(1), predicate));
            op->computeFlatSchema();
            return op;
        }
    } break;
    default:
        break;
    }
    return appendFilter(std::move(op), predicate);
}

std::shared_ptr<LogicalOperator> GroupKeyPredicatePushDownOptimizer::appendFilter(
    std::shared_ptr<LogicalOperator> child, const std::shared_ptr<Expression>& predicate,
    cardinality_t cardinality) {
    auto filter = std::make_shared<LogicalFilter>(predicate, std::move(child), cardinality);
    filter->computeFlatSchema();
    return filter;
}

void GroupKeyPredicatePushDownOptimizer::recomputeFlatSchemas(
    const std::shared_ptr<LogicalOperator>& op) {
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        recomputeFlatSchemas(op->getChild(i));
    }
    op->computeFlatSchema();
}

} // namespace optimizer
} // namespace lbug

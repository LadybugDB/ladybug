#include "optimizer/eager_count_optimizer.h"

#include <algorithm>
#include <unordered_set>

#include "binder/binder.h"
#include "binder/expression/aggregate_function_expression.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/function_catalog_entry.h"
#include "function/aggregate/count_star.h"
#include "function/aggregate_function.h"
#include "function/arithmetic/vector_arithmetic_functions.h"
#include "function/built_in_function_utils.h"
#include "function/utility/vector_utility_functions.h"
#include "main/client_context.h"
#include "planner/operator/extend/logical_extend.h"
#include "planner/operator/logical_aggregate.h"
#include "planner/operator/logical_filter.h"
#include "planner/operator/logical_hash_join.h"
#include "planner/operator/logical_projection.h"
#include "planner/operator/scan/logical_scan_node_table.h"
#include "transaction/transaction.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::planner;

namespace lbug {
namespace optimizer {

void EagerCountOptimizer::rewrite(LogicalPlan* plan) {
    visitOperator(plan->getLastOperator());
}

std::shared_ptr<LogicalOperator> EagerCountOptimizer::visitOperator(
    const std::shared_ptr<LogicalOperator>& op) {
    // bottom-up traversal
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        op->setChild(i, visitOperator(op->getChild(i)));
    }
    auto result = visitOperatorReplaceSwitch(op);
    result->computeFlatSchema();
    return result;
}

static bool isKeylessCountStar(const LogicalAggregate& aggregate) {
    if (aggregate.hasKeys() || !aggregate.getDependentKeys().empty() ||
        aggregate.getAggregates().size() != 1) {
        return false;
    }
    auto& expression = *aggregate.getAggregates()[0];
    if (expression.expressionType != ExpressionType::AGGREGATE_FUNCTION) {
        return false;
    }
    auto& function = expression.constCast<AggregateFunctionExpression>();
    return function.getFunction().name == CountStarFunction::name && !function.isDistinct();
}

static bool containsExtend(const LogicalOperator& op) {
    if (op.getOperatorType() == LogicalOperatorType::EXTEND) {
        return true;
    }
    for (auto i = 0u; i < op.getNumChildren(); ++i) {
        if (containsExtend(*op.getChild(i))) {
            return true;
        }
    }
    return false;
}

// True when nothing above the join can read a build column other than the join keys, and the
// build side can hold several rows per key.
static bool canPreAggregateBuildSide(const LogicalHashJoin& join) {
    auto& build = *join.getChild(1);
    if (build.getOperatorType() != LogicalOperatorType::PROJECTION || !containsExtend(build)) {
        return false;
    }
    std::unordered_set<std::string> keys;
    for (auto& [probeKey, buildKey] : join.getJoinConditions()) {
        keys.insert(buildKey->getUniqueName());
    }
    std::unordered_set<std::string> projected;
    for (auto& expression : build.constCast<LogicalProjection>().getExpressionsToProject()) {
        projected.insert(expression->getUniqueName());
    }
    return keys == projected;
}

// True when nothing under op narrows the nodes it reaches. Such a probe side leaves a semi mask
// little of a pre-aggregated build side to prune, which does not pay for materializing the probe
// side. Inequalities between node IDs remove next to no rows.
static bool isUnselective(const LogicalOperator& op) {
    switch (op.getOperatorType()) {
    case LogicalOperatorType::SCAN_NODE_TABLE: {
        auto& scan = op.constCast<LogicalScanNodeTable>();
        return scan.getScanType() == LogicalScanNodeTableType::SCAN &&
               std::ranges::all_of(scan.getPropertyPredicates(),
                   [](const auto& predicates) { return predicates.isEmpty(); });
    }
    case LogicalOperatorType::EXTEND:
        return std::ranges::all_of(op.constCast<LogicalExtend>().getPropertyPredicates(),
                   [](const auto& predicates) { return predicates.isEmpty(); }) &&
               isUnselective(*op.getChild(0));
    case LogicalOperatorType::PROJECTION:
        return isUnselective(*op.getChild(0));
    case LogicalOperatorType::FILTER:
        return op.constCast<LogicalFilter>().getPredicate()->expressionType ==
                   ExpressionType::NOT_EQUALS &&
               isUnselective(*op.getChild(0));
    case LogicalOperatorType::HASH_JOIN:
        return isUnselective(*op.getChild(0)) && isUnselective(*op.getChild(1));
    default:
        return false;
    }
}

// Only these operators keep every row's contribution to the count independent of the columns
// that pre-aggregation removes.
static bool canRewriteSubtree(const LogicalOperator& op, bool& hasBuildSideToPreAggregate) {
    switch (op.getOperatorType()) {
    case LogicalOperatorType::SCAN_NODE_TABLE:
        return true;
    case LogicalOperatorType::PROJECTION:
    case LogicalOperatorType::FILTER:
    case LogicalOperatorType::EXTEND:
        return canRewriteSubtree(*op.getChild(0), hasBuildSideToPreAggregate);
    case LogicalOperatorType::HASH_JOIN: {
        auto& join = op.constCast<LogicalHashJoin>();
        if (join.getJoinType() != JoinType::INNER || join.hasMark() ||
            !LogicalHashJoin::isNodeIDOnlyJoin(join.getJoinConditions())) {
            return false;
        }
        hasBuildSideToPreAggregate |= canPreAggregateBuildSide(join);
        return canRewriteSubtree(*op.getChild(0), hasBuildSideToPreAggregate) &&
               canRewriteSubtree(*op.getChild(1), hasBuildSideToPreAggregate);
    }
    default:
        return false;
    }
}

std::string EagerCountOptimizer::getMultiplicityName() {
    return "__eager_count_" + std::to_string(numMultiplicities++);
}

static std::shared_ptr<Expression> bindAggregate(main::ClientContext* context,
    const std::string& name, expression_vector children, std::string uniqueName) {
    std::vector<LogicalType> childrenTypes;
    for (auto& child : children) {
        childrenTypes.push_back(child->getDataType().copy());
    }
    auto entry = catalog::Catalog::Get(*context)->getFunctionEntry(
        transaction::Transaction::Get(*context), name);
    auto function = BuiltInFunctionsUtils::matchAggregateFunction(name, childrenTypes,
        false /* isDistinct */, entry->ptrCast<catalog::FunctionCatalogEntry>());
    auto bindData = std::make_unique<FunctionBindData>(LogicalType(function->returnTypeID));
    return std::make_shared<AggregateFunctionExpression>(function->copy(), std::move(bindData),
        std::move(children), std::move(uniqueName));
}

std::shared_ptr<Expression> EagerCountOptimizer::bindProduct(const expression_vector& factors) {
    auto binder = Binder(context);
    auto* expressionBinder = binder.getExpressionBinder();
    std::shared_ptr<Expression> product;
    for (auto& factor : factors) {
        auto castFactor = expressionBinder->implicitCastIfNecessary(factor, LogicalType::INT128());
        product = product == nullptr ? castFactor :
                                       expressionBinder->bindScalarFunctionExpression(
                                           {product, castFactor}, MultiplyFunction::name);
    }
    return product;
}

std::shared_ptr<LogicalAggregate> EagerCountOptimizer::aggregateMultiplicities(
    expression_vector keys, const expression_vector& multiplicities,
    std::shared_ptr<LogicalOperator> child) {
    const auto cardinality = child->getCardinality();
    std::shared_ptr<Expression> aggregateExpr;
    if (multiplicities.empty()) {
        aggregateExpr = bindAggregate(context, CountStarFunction::name, expression_vector{},
            getMultiplicityName());
    } else {
        // An aggregate reads its argument from a column of its input.
        auto product = bindProduct(multiplicities);
        auto projected = keys;
        projected.push_back(product);
        child = std::make_shared<LogicalProjection>(std::move(projected), std::move(child));
        child->computeFlatSchema();
        aggregateExpr = bindAggregate(context, AggregateSumFunction::name,
            expression_vector{std::move(product)}, getMultiplicityName());
    }
    auto aggregate = std::make_shared<LogicalAggregate>(std::move(keys), expression_vector{},
        expression_vector{std::move(aggregateExpr)}, std::move(child), cardinality);
    aggregate->computeFlatSchema();
    return aggregate;
}

binder::expression_vector EagerCountOptimizer::preAggregateBuildSides(LogicalOperator* op) {
    expression_vector multiplicities;
    switch (op->getOperatorType()) {
    case LogicalOperatorType::PROJECTION: {
        multiplicities = preAggregateBuildSides(op->getChild(0).get());
        auto& projection = op->cast<LogicalProjection>();
        auto expressions = projection.getExpressionsToProject();
        expressions.insert(expressions.end(), multiplicities.begin(), multiplicities.end());
        projection.setExpressionsToProject(expressions);
    } break;
    case LogicalOperatorType::FILTER:
    case LogicalOperatorType::EXTEND: {
        multiplicities = preAggregateBuildSides(op->getChild(0).get());
    } break;
    case LogicalOperatorType::HASH_JOIN: {
        auto& join = op->cast<LogicalHashJoin>();
        const auto preAggregate = canPreAggregateBuildSide(join);
        const auto skipProbeToBuildSIP = preAggregate && isUnselective(*op->getChild(0));
        multiplicities = preAggregateBuildSides(op->getChild(0).get());
        auto buildMultiplicities = preAggregateBuildSides(op->getChild(1).get());
        if (preAggregate) {
            expression_vector keys;
            for (auto& [probeKey, buildKey] : join.getJoinConditions()) {
                keys.push_back(buildKey);
            }
            auto aggregate =
                aggregateMultiplicities(std::move(keys), buildMultiplicities, op->getChild(1));
            buildMultiplicities = aggregate->getAggregates();
            op->setChild(1, std::move(aggregate));
            if (skipProbeToBuildSIP && join.getSIPInfo().position == SemiMaskPosition::NONE) {
                join.getSIPInfoUnsafe().position = SemiMaskPosition::PROHIBIT_PROBE_TO_BUILD;
            }
        }
        multiplicities.insert(multiplicities.end(), buildMultiplicities.begin(),
            buildMultiplicities.end());
    } break;
    default:
        break;
    }
    op->computeFlatSchema();
    return multiplicities;
}

std::shared_ptr<LogicalOperator> EagerCountOptimizer::visitAggregateReplace(
    std::shared_ptr<LogicalOperator> op) {
    auto& aggregate = op->constCast<LogicalAggregate>();
    if (!isKeylessCountStar(aggregate)) {
        return op;
    }
    auto hasBuildSideToPreAggregate = false;
    if (!canRewriteSubtree(*op->getChild(0), hasBuildSideToPreAggregate) ||
        !hasBuildSideToPreAggregate) {
        return op;
    }
    auto count = aggregate.getAggregates()[0];
    auto child = op->getChild(0);
    auto newAggregate =
        aggregateMultiplicities(expression_vector{}, preAggregateBuildSides(child.get()), child);
    auto sum = newAggregate->getAggregates()[0];

    // COUNT(*) is INT64 and zero over no rows, whereas SUM is INT128 and NULL over no rows.
    auto binder = Binder(context);
    auto* expressionBinder = binder.getExpressionBinder();
    auto castSum = expressionBinder->forceCast(sum, count->getDataType());
    auto zero = expressionBinder->createLiteralExpression(Value{int64_t{0}});
    auto total =
        expressionBinder->bindScalarFunctionExpression({castSum, zero}, CoalesceFunction::name);
    total->setUniqueName(count->getUniqueName());
    if (count->hasAlias()) {
        total->setAlias(count->getAlias());
    }
    auto projection =
        std::make_shared<LogicalProjection>(expression_vector{std::move(total)}, newAggregate);
    projection->computeFlatSchema();
    return projection;
}

} // namespace optimizer
} // namespace lbug

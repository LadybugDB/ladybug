#include <unordered_set>

#include "binder/expression/expression_util.h"
#include "binder/expression/literal_expression.h"
#include "binder/expression/variable_expression.h"
#include "common/types/value/value.h"
#include "gtest/gtest.h"
#include "optimizer/limit_push_down_optimizer.h"
#include "planner/operator/logical_accumulate.h"
#include "planner/operator/logical_aggregate.h"
#include "planner/operator/logical_cross_product.h"
#include "planner/operator/logical_hash_join.h"
#include "planner/operator/logical_limit.h"
#include "planner/operator/logical_multiplcity_reducer.h"
#include "planner/operator/logical_order_by.h"
#include "planner/operator/logical_plan.h"
#include "planner/operator/logical_union.h"
#include "planner/operator/operator_properties.h"
#include "planner/operator/scan/logical_dummy_scan.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace testing {

TEST(OperatorPropertiesTest, AllLogicalOperatorsAreUniqueAndAlgebraicallyConsistent) {
    std::unordered_set<uint8_t> seen;
    for (auto type : ALL_LOGICAL_OPERATOR_TYPES) {
        ASSERT_TRUE(seen.insert(static_cast<uint8_t>(type)).second)
            << "Duplicate operator in ALL_LOGICAL_OPERATOR_TYPES: "
            << LogicalOperatorUtils::logicalOperatorTypeToString(type);
        const auto traits = getStaticOperatorTraits(type);
        const auto name = LogicalOperatorUtils::logicalOperatorTypeToString(type);

        // Invariant 1: Side-effecting operators (writes, DDL, I/O) must be hard pushdown barriers.
        if (traits.sideEffect != SideEffectClass::PURE) {
            EXPECT_EQ(traits.limitAction, LimitPushDownAction::STOP) << name;
            EXPECT_EQ(traits.orderByAction, OrderByPushDownAction::STOP) << name;
            EXPECT_EQ(traits.filterAction, FilterPushDownAction::STOP) << name;
        }

        // Invariant 2: ORDER BY pass-through requires strict order preservation and must never
        // pass through LIMIT (ORDER BY does not commute with LIMIT).
        if (traits.orderByAction == OrderByPushDownAction::PASS_THROUGH_CHILD_0) {
            EXPECT_EQ(traits.orderPreservation, OrderPreservation::PRESERVES_ORDER) << name;
            EXPECT_NE(type, LogicalOperatorType::LIMIT) << name;
            EXPECT_EQ(traits.pipelineBehavior, PipelineBehavior::STREAMING) << name;
        }

        // Invariant 3: Transparent LIMIT pass-through requires a pure streaming operator that
        // never drops input rows (EXACT_ONE or AT_LEAST_ONE).
        if (traits.limitAction == LimitPushDownAction::PASS_THROUGH_CHILD_0) {
            EXPECT_EQ(traits.sideEffect, SideEffectClass::PURE) << name;
            EXPECT_EQ(traits.pipelineBehavior, PipelineBehavior::STREAMING) << name;
            EXPECT_TRUE(traits.child0Cardinality == RowCardinalityEffect::EXACT_ONE ||
                        traits.child0Cardinality == RowCardinalityEffect::AT_LEAST_ONE)
                << name;
        }

        // Invariant 4: LIMIT itself must block ORDER BY and FILTER pushdown.
        if (type == LogicalOperatorType::LIMIT) {
            EXPECT_EQ(traits.limitAction, LimitPushDownAction::COMBINE_LIMIT);
            EXPECT_EQ(traits.orderByAction, OrderByPushDownAction::STOP);
            EXPECT_EQ(traits.filterAction, FilterPushDownAction::STOP);
        }

        // Invariant 5: ACCUMULATE breaks order (multi-threaded merge) and must block ORDER BY.
        if (type == LogicalOperatorType::ACCUMULATE) {
            EXPECT_EQ(traits.orderPreservation, OrderPreservation::BREAKS_ORDER);
            EXPECT_EQ(traits.orderByAction, OrderByPushDownAction::STOP);
            EXPECT_EQ(traits.pipelineBehavior, PipelineBehavior::PIPELINE_BREAKER);
        }

        // Invariant 6: FILTER absorbs predicates; DISTINCT only splits on keys.
        if (type == LogicalOperatorType::FILTER) {
            EXPECT_EQ(traits.filterAction, FilterPushDownAction::ABSORB_FILTER);
        }
        if (type == LogicalOperatorType::DISTINCT) {
            EXPECT_EQ(traits.filterAction, FilterPushDownAction::SPLIT_ON_GROUP_KEYS);
        }
    }
}

TEST(OperatorPropertiesTest, ExhaustiveJoinAndAccumulateHelpers) {
    EXPECT_FALSE(joinNullsBuildOnlyColumns(JoinType::INNER));
    EXPECT_FALSE(joinNullsBuildOnlyColumns(JoinType::MARK));
    EXPECT_FALSE(joinNullsBuildOnlyColumns(JoinType::COUNT));
    EXPECT_TRUE(joinNullsBuildOnlyColumns(JoinType::LEFT));

    EXPECT_EQ(joinProbeCardinalityEffect(JoinType::INNER), RowCardinalityEffect::ZERO_OR_MORE);
    EXPECT_EQ(joinProbeCardinalityEffect(JoinType::LEFT), RowCardinalityEffect::AT_LEAST_ONE);
    EXPECT_EQ(joinProbeCardinalityEffect(JoinType::MARK), RowCardinalityEffect::EXACT_ONE);
    EXPECT_EQ(joinProbeCardinalityEffect(JoinType::COUNT), RowCardinalityEffect::EXACT_ONE);

    EXPECT_FALSE(isOptionalAccumulate(AccumulateType::REGULAR));
    EXPECT_TRUE(isOptionalAccumulate(AccumulateType::OPTIONAL_));
}

TEST(OperatorPropertiesTest, DynamicSemanticsForJoinsAccumulatesOrderByAndAggregate) {
    auto dummy1 = std::make_shared<LogicalDummyScan>();
    dummy1->computeFlatSchema();
    auto dummy2 = std::make_shared<LogicalDummyScan>();
    dummy2->computeFlatSchema();
    auto varA = std::make_shared<VariableExpression>(LogicalType::INT64(), "a", "a");
    auto varB = std::make_shared<VariableExpression>(LogicalType::INT64(), "b", "b");
    auto mark = std::make_shared<VariableExpression>(LogicalType::BOOL(), "m", "m");

    // HASH_JOIN by JoinType
    {
        LogicalHashJoin leftJoin({{varA, varB}}, JoinType::LEFT, nullptr, dummy1, dummy2);
        auto sem = analyzeOperator(leftJoin);
        EXPECT_EQ(sem.child0Cardinality, RowCardinalityEffect::AT_LEAST_ONE);
        EXPECT_EQ(sem.limitAction, LimitPushDownAction::PUSH_TO_PROBE_CHILD);
        EXPECT_EQ(sem.nullSupply, NullSupplyBehavior::MAY_NULL_BUILD_ONLY);

        LogicalHashJoin markedLeft({{varA, varB}}, JoinType::LEFT, mark, dummy1, dummy2);
        EXPECT_EQ(analyzeOperator(markedLeft).nullSupply, NullSupplyBehavior::NEVER_NULLS);

        LogicalHashJoin markJoin({{varA, varB}}, JoinType::MARK, mark, dummy1, dummy2);
        EXPECT_EQ(analyzeOperator(markJoin).child0Cardinality, RowCardinalityEffect::EXACT_ONE);
        EXPECT_EQ(analyzeOperator(markJoin).limitAction, LimitPushDownAction::PUSH_TO_PROBE_CHILD);

        LogicalHashJoin countJoin({{varA, varB}}, JoinType::COUNT, nullptr, dummy1, dummy2);
        EXPECT_EQ(analyzeOperator(countJoin).child0Cardinality, RowCardinalityEffect::EXACT_ONE);
        EXPECT_EQ(analyzeOperator(countJoin).limitAction, LimitPushDownAction::PUSH_TO_PROBE_CHILD);

        LogicalHashJoin innerJoin({{varA, varB}}, JoinType::INNER, nullptr, dummy1, dummy2);
        EXPECT_EQ(analyzeOperator(innerJoin).limitAction, LimitPushDownAction::STOP);
    }

    // ACCUMULATE REGULAR vs OPTIONAL_
    {
        LogicalAccumulate regAcc(AccumulateType::REGULAR, {}, nullptr, dummy1);
        auto regSem = analyzeOperator(regAcc);
        EXPECT_EQ(regSem.child0Cardinality, RowCardinalityEffect::EXACT_ONE);
        EXPECT_EQ(regSem.nullSupply, NullSupplyBehavior::NEVER_NULLS);
        EXPECT_EQ(regSem.limitAction, LimitPushDownAction::CAP_AND_PASS_THROUGH_CHILD_0);
        EXPECT_EQ(regSem.filterAction, FilterPushDownAction::PASS_THROUGH_IF_EVALUABLE);

        LogicalAccumulate optAcc(AccumulateType::OPTIONAL_, {}, nullptr, dummy1);
        auto optSem = analyzeOperator(optAcc);
        EXPECT_EQ(optSem.child0Cardinality, RowCardinalityEffect::GLOBAL);
        EXPECT_EQ(optSem.nullSupply, NullSupplyBehavior::MAY_NULL_ANY);
        EXPECT_EQ(optSem.limitAction, LimitPushDownAction::CAP_AND_PASS_THROUGH_CHILD_0);
        EXPECT_EQ(optSem.filterAction, FilterPushDownAction::STOP);
    }

    // CROSS_PRODUCT REGULAR vs OPTIONAL_
    {
        LogicalCrossProduct regCP(AccumulateType::REGULAR, nullptr, dummy1, dummy2, 1);
        EXPECT_EQ(analyzeOperator(regCP).nullSupply, NullSupplyBehavior::NEVER_NULLS);
        EXPECT_EQ(analyzeOperator(regCP).limitAction, LimitPushDownAction::PUSH_TO_ALL_CHILDREN);

        LogicalCrossProduct optCP(AccumulateType::OPTIONAL_, nullptr, dummy1, dummy2, 1);
        EXPECT_EQ(analyzeOperator(optCP).nullSupply, NullSupplyBehavior::MAY_NULL_BUILD_ONLY);
        EXPECT_EQ(analyzeOperator(optCP).child0Cardinality, RowCardinalityEffect::AT_LEAST_ONE);
    }

    // ORDER_BY without vs with LIMIT
    {
        LogicalOrderBy orderBy({varA}, {true}, dummy1);
        auto unboundSem = analyzeOperator(orderBy);
        EXPECT_EQ(unboundSem.limitAction, LimitPushDownAction::ABSORB_NATIVE);
        EXPECT_EQ(unboundSem.orderByAction, OrderByPushDownAction::COMBINE_ORDER_BY);
        EXPECT_EQ(unboundSem.filterAction, FilterPushDownAction::PASS_THROUGH_IF_EVALUABLE);

        orderBy.setLimitNum(std::make_shared<LiteralExpression>(Value(int64_t{5}), "_lim"));
        auto boundSem = analyzeOperator(orderBy);
        EXPECT_EQ(boundSem.limitAction, LimitPushDownAction::STOP);
        EXPECT_EQ(boundSem.orderByAction, OrderByPushDownAction::STOP);
        EXPECT_EQ(boundSem.filterAction, FilterPushDownAction::STOP);
    }

    // AGGREGATE keyless vs keyed
    {
        LogicalAggregate keyless({}, {}, dummy1);
        EXPECT_EQ(analyzeOperator(keyless).filterAction, FilterPushDownAction::STOP);

        LogicalAggregate keyed({varA}, {}, dummy1);
        EXPECT_EQ(analyzeOperator(keyed).filterAction, FilterPushDownAction::SPLIT_ON_GROUP_KEYS);
    }
}

TEST(OperatorPropertiesTest, LimitPushDownCapsUnionAllBranchesAndFusesOrderBy) {
    auto dummy1 = std::make_shared<LogicalDummyScan>();
    dummy1->computeFlatSchema();
    auto dummy2 = std::make_shared<LogicalDummyScan>();
    dummy2->computeFlatSchema();
    auto varA = std::make_shared<VariableExpression>(LogicalType::INT64(), "a", "a");
    auto orderBy =
        std::make_shared<LogicalOrderBy>(expression_vector{varA}, std::vector<bool>{true}, dummy2);
    orderBy->computeFlatSchema();

    auto unionOp = std::make_shared<LogicalUnion>(expression_vector{},
        std::vector<std::shared_ptr<LogicalOperator>>{dummy1, orderBy});
    unionOp->computeFlatSchema();
    auto reducer = std::make_shared<LogicalMultiplicityReducer>(unionOp);
    reducer->computeFlatSchema();
    auto limitOp = std::make_shared<LogicalLimit>(
        std::make_shared<LiteralExpression>(Value(int64_t{2}), "_skip"),
        std::make_shared<LiteralExpression>(Value(int64_t{5}), "_lim"), reducer);
    limitOp->computeFlatSchema();

    LogicalPlan plan;
    plan.setLastOperator(limitOp);
    optimizer::LimitPushDownOptimizer opt;
    opt.rewrite(&plan);

    // Branch 0 (non-native leaf) gets capped with LIMIT(2 + 5 = 7).
    EXPECT_EQ(unionOp->getChild(0)->getOperatorType(), LogicalOperatorType::LIMIT);
    // Branch 1 (ORDER_BY) absorbs the cap directly as TOP_K(7).
    EXPECT_EQ(unionOp->getChild(1)->getOperatorType(), LogicalOperatorType::ORDER_BY);
    auto& rewrittenOrderBy = unionOp->getChild(1)->constCast<LogicalOrderBy>();
    ASSERT_TRUE(rewrittenOrderBy.hasLimitNum());
    EXPECT_EQ(ExpressionUtil::evaluateAsSkipLimit(*rewrittenOrderBy.getLimitNum()), 7u);
}

} // namespace testing
} // namespace lbug

#include <string>
#include <vector>

#include "graph_test/private_graph_test.h"
#include "planner/operator/logical_operator.h"
#include "planner/operator/logical_unwind.h"
#include "planner/planner.h"
#include "test_helper/test_helper.h"
#include "test_runner/test_runner.h"

namespace lbug {
namespace testing {

// Unit tests for correlated OPTIONAL MATCH planning (see plan_subquery.cpp):
// the collect-membership unnest (IN over a COLLECT-built list), the unnest-vs-correlated
// branch decision, and the single-node PK lookup fast path. Plan-shape tests use
// tinysnb; the branch policy itself is tested directly through
// CorrelatedOptionalLegDecision (no DB needed).
class OptionalMatchStrategyTest : public DBTest {
public:
    std::string getInputDir() override {
        return TestHelper::appendLbugRootPath("dataset/tinysnb/");
    }

    std::unique_ptr<planner::LogicalPlan> getRoot(const std::string& query) {
        return TestRunner::getLogicalPlan(query, *conn);
    }

    static void collectOps(planner::LogicalOperator* op, planner::LogicalOperatorType type,
        std::vector<planner::LogicalOperator*>& out) {
        if (op->getOperatorType() == type) {
            out.push_back(op);
        }
        for (auto i = 0u; i < op->getNumChildren(); ++i) {
            collectOps(op->getChild(i).get(), type, out);
        }
    }

    // The rewrite unwinds the COLLECT output itself, so its UNWIND input is the aggregate
    // function expression; explicit query UNWINDs consume literals, parameters, or
    // other expressions instead. Matching on the input kind identifies the rewrite
    // regardless of binder-assigned unique names.
    static bool hasCollectUnwind(planner::LogicalOperator* root) {
        std::vector<planner::LogicalOperator*> unwinds;
        collectOps(root, planner::LogicalOperatorType::UNWIND, unwinds);
        for (auto* op : unwinds) {
            auto& unwind = op->constCast<planner::LogicalUnwind>();
            if (unwind.getInExpr()->expressionType == common::ExpressionType::AGGREGATE_FUNCTION) {
                return true;
            }
        }
        return false;
    }

    static bool hasOp(planner::LogicalOperator* root, planner::LogicalOperatorType type) {
        std::vector<planner::LogicalOperator*> found;
        collectOps(root, type, found);
        return !found.empty();
    }
};

using Strategy = planner::CorrelatedOptionalLegDecision::Strategy;

TEST_F(OptionalMatchStrategyTest, DecidePkLookupTakesPrecedenceOverCrossVarUnnest) {
    // LDBC SNB complex-7 tail shape: single-node PK leg whose key is cross-variable. The keyed
    // lookup must win over the cross-variable unnest override.
    planner::CorrelatedOptionalLegDecision decision;
    decision.canUnnest = true;
    decision.hasCrossVariableJoin = true;
    decision.pkLookupEligible = true;
    EXPECT_EQ(decision.decide(), Strategy::PRIMARY_KEY_LOOKUP);
}

TEST_F(OptionalMatchStrategyTest, DecideLegacyPathSkipsPkLookup) {
    // MERGE existence checks / join hints keep pre-change behavior: unnest when the
    // legacy analysis allows it, never the PK fast path.
    planner::CorrelatedOptionalLegDecision decision;
    decision.legacyPath = true;
    decision.canUnnest = true;
    decision.hasCrossVariableJoin = true;
    decision.pkLookupEligible = true;
    EXPECT_EQ(decision.decide(), Strategy::UNNEST_LEFT_JOIN);
}

TEST_F(OptionalMatchStrategyTest, DecideCrossVarUnnestWithoutPk) {
    // Collect-membership shape: cross-variable equality, no PK fast path available.
    planner::CorrelatedOptionalLegDecision decision;
    decision.canUnnest = true;
    decision.hasCrossVariableJoin = true;
    EXPECT_EQ(decision.decide(), Strategy::UNNEST_LEFT_JOIN);
}

TEST_F(OptionalMatchStrategyTest, DecideInnerSelectiveUnnests) {
    // LDBC SNB complex-14 shape: every correlated node constant-filtered in-leg.
    planner::CorrelatedOptionalLegDecision decision;
    decision.canUnnest = true;
    decision.innerSelective = true;
    EXPECT_EQ(decision.decide(), Strategy::UNNEST_LEFT_JOIN);
}

TEST_F(OptionalMatchStrategyTest, DecideSameVarOuterDrivenStaysCorrelated) {
    // LDBC SNB complex-7 like-branch shape: same-variable conditions only, selectivity comes from
    // outer bindings (semi masks), so correlated execution wins.
    planner::CorrelatedOptionalLegDecision decision;
    decision.canUnnest = true;
    EXPECT_EQ(decision.decide(), Strategy::CORRELATED);
}

TEST_F(OptionalMatchStrategyTest, DecideRecursiveVetoesUnnest) {
    planner::CorrelatedOptionalLegDecision decision;
    decision.canUnnest = true;
    decision.innerSelective = true;
    decision.hasCrossVariableJoin = true;
    decision.hasRecursiveRel = true;
    EXPECT_EQ(decision.decide(), Strategy::CORRELATED);
}

TEST_F(OptionalMatchStrategyTest, DecideRecursiveVetoesPkLookup) {
    planner::CorrelatedOptionalLegDecision decision;
    decision.pkLookupEligible = true;
    decision.hasRecursiveRel = true;
    EXPECT_EQ(decision.decide(), Strategy::CORRELATED);
}

TEST_F(OptionalMatchStrategyTest, DecideUnanalyzableStaysCorrelated) {
    planner::CorrelatedOptionalLegDecision decision;
    decision.innerSelective = true;
    decision.hasCrossVariableJoin = true;
    EXPECT_EQ(decision.decide(), Strategy::CORRELATED);
}

TEST_F(OptionalMatchStrategyTest, DecidePkLookupNeedsNoUnnestAnalysis) {
    // The keyed lookup path never consults the predicate analysis: a single-node PK
    // leg with an outer-dependent key wins even when nothing is unnestable.
    planner::CorrelatedOptionalLegDecision decision;
    decision.pkLookupEligible = true;
    EXPECT_EQ(decision.decide(), Strategy::PRIMARY_KEY_LOOKUP);
}

TEST_F(OptionalMatchStrategyTest, CollectMembershipUnnestFires) {
    // Collect-membership shape (cf. LDBC SNB complex-5): the IN-list becomes
    // (group key, element) rows + an equality that hash-joins, so the leg plans
    // standalone (no EXPRESSIONS_SCAN) with no residual filter.
    auto root = getRoot("MATCH (p:person)-[:knows]->(f:person) WITH p, COLLECT(f) AS friends "
                        "OPTIONAL MATCH (a:person)-[:knows]->(b:person) WHERE b IN friends "
                        "RETURN p.ID, COUNT(a)");
    EXPECT_TRUE(hasCollectUnwind(root->getLastOperator().get()));
    EXPECT_FALSE(
        hasOp(root->getLastOperator().get(), planner::LogicalOperatorType::EXPRESSIONS_SCAN));
}

TEST_F(OptionalMatchStrategyTest, CollectMembershipSkippedAfterLeftJoin) {
    // The collect subtree is nullable (LEFT join below the aggregate), so the rewrite
    // must not fire: no UNWIND over the list, leg stays correlated.
    auto root = getRoot("MATCH (p:person) OPTIONAL MATCH (p)-[:knows]->(f:person) "
                        "WITH p, COLLECT(f) AS friends "
                        "OPTIONAL MATCH (a:person)-[:knows]->(b:person) WHERE b IN friends "
                        "RETURN p.ID, COUNT(a)");
    EXPECT_FALSE(hasCollectUnwind(root->getLastOperator().get()));
    EXPECT_TRUE(
        hasOp(root->getLastOperator().get(), planner::LogicalOperatorType::EXPRESSIONS_SCAN));
}

TEST_F(OptionalMatchStrategyTest, CollectMembershipSkippedAfterUnwind) {
    // UNWIND is conservatively classified as potentially null-supplying (its list
    // could carry nulls): an exotic operator below the aggregate disables the rewrite
    // rather than risking wrong results. The query's own UNWIND consumes a literal
    // list, so any aggregate-input UNWIND found would be the rewrite's.
    auto root = getRoot("UNWIND [1, 2, 3] AS k MATCH (f:person) WHERE f.ID > k "
                        "WITH k, COLLECT(f) AS friends "
                        "OPTIONAL MATCH (a:person)-[:knows]->(b:person) WHERE b IN friends "
                        "RETURN k, COUNT(a)");
    EXPECT_FALSE(hasCollectUnwind(root->getLastOperator().get()));
}

TEST_F(OptionalMatchStrategyTest, CrossVariableEqualityUnnests) {
    auto root = getRoot("MATCH (a:person) WITH a "
                        "OPTIONAL MATCH (b:person)-[:knows]->(c:person) WHERE c.ID = a.ID "
                        "RETURN a.ID, COUNT(c)");
    EXPECT_FALSE(
        hasOp(root->getLastOperator().get(), planner::LogicalOperatorType::EXPRESSIONS_SCAN));
}

TEST_F(OptionalMatchStrategyTest, SameVariableOuterDrivenStaysCorrelated) {
    auto root = getRoot("MATCH (a:person) WITH a "
                        "OPTIONAL MATCH (a)-[:knows]->(b:person) "
                        "RETURN a.ID, COUNT(b)");
    EXPECT_TRUE(
        hasOp(root->getLastOperator().get(), planner::LogicalOperatorType::EXPRESSIONS_SCAN));
}

TEST_F(OptionalMatchStrategyTest, NullFreeOperatorClassification) {
    // Exhaustive table for isNullFreeOperator: every enumerator classified, so adding
    // a new operator type forces an explicit decision here too (the switch itself is
    // -Wswitch-enforced with no default label; see #935). True = passes through,
    // filters, or combines already-bound rows and cannot introduce null bindings.
    using T = planner::LogicalOperatorType;
    for (auto type : {T::AGGREGATE, T::COUNT_ANTI_EDGE_CHAIN, T::COUNT_EXTEND_CHAIN,
             T::COUNT_REL_TABLE, T::CROSS_PRODUCT, T::DISTINCT, T::DUMMY_SCAN, T::EMPTY_RESULT,
             T::EXTEND, T::FILTER, T::FLATTEN, T::INDEX_LOOK_UP, T::INTERSECT, T::LIMIT,
             T::MULTIPLICITY_REDUCER, T::NODE_LABEL_FILTER, T::ORDER_BY, T::PACKED_EXTEND,
             T::PATH_PROPERTY_PROBE, T::PROJECTION, T::QUERY_PRIMARY_KEY_LOOKUP, T::REACHABLE_COUNT,
             T::RECURSIVE_EXTEND, T::REL_DEGREE_TABLE, T::SCAN_NODE_TABLE, T::SEMI_MASKER}) {
        EXPECT_TRUE(planner::isNullFreeOperator(type))
            << "expected null-free: "
            << planner::LogicalOperatorUtils::logicalOperatorTypeToString(type);
    }
    for (auto type : {T::ACCUMULATE, T::ALTER, T::ANALYZE, T::ATTACH_DATABASE, T::COPY_FROM,
             T::COPY_TO, T::CREATE_GRAPH, T::CREATE_INDEX, T::CREATE_MACRO, T::CREATE_SEQUENCE,
             T::CREATE_TABLE, T::CREATE_TYPE, T::DELETE, T::DETACH_DATABASE, T::DROP, T::DUMMY_SINK,
             T::EXPLAIN, T::EXPRESSIONS_SCAN, T::EXTENSION, T::EXTENSION_CLAUSE, T::EXPORT_DATABASE,
             T::HASH_JOIN, T::IMPORT_DATABASE, T::INSERT, T::MERGE, T::NOOP, T::PARTITIONER,
             T::SET_PROPERTY, T::STANDALONE_CALL, T::TABLE_FUNCTION_CALL, T::TRANSACTION,
             T::UNION_ALL, T::UNWIND, T::UNWIND_DEDUPLICATE, T::USE_DATABASE, T::USE_GRAPH}) {
        EXPECT_FALSE(planner::isNullFreeOperator(type))
            << "expected null-supplying: "
            << planner::LogicalOperatorUtils::logicalOperatorTypeToString(type);
    }
}

TEST_F(OptionalMatchStrategyTest, SingleNodePkLegUsesKeyedLookup) {
    // LDBC SNB complex-7 tail shape: must keep the PK fast path (and not be stolen by unnesting,
    // whose key is cross-variable here).
    auto root = getRoot("MATCH (a:person) WITH a "
                        "OPTIONAL MATCH (b:person {ID: a.ID}) "
                        "RETURN a.ID, b.fName");
    EXPECT_TRUE(hasOp(root->getLastOperator().get(),
        planner::LogicalOperatorType::QUERY_PRIMARY_KEY_LOOKUP));
}

} // namespace testing
} // namespace lbug

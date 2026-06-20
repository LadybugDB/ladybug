#include <functional>
#include <stdexcept>

#include "graph_test/private_graph_test.h"
#include "planner/join_order/cost_model.h"
#include "planner/operator/logical_filter.h"
#include "planner/operator/logical_plan_util.h"
#include "planner/operator/scan/logical_count_rel_table.h"
#include "planner/operator/scan/logical_dummy_scan.h"
#include "test_runner/test_runner.h"
#include <format>

namespace lbug {
namespace testing {

class OptimizerTest : public DBTest {
public:
    std::string getInputDir() override {
        return TestHelper::appendLbugRootPath("dataset/tinysnb/");
    }

    std::string getEncodedPlan(const std::string& query) {
        return planner::LogicalPlanUtil::encodeJoin(*TestRunner::getLogicalPlan(query, *conn));
    }
    std::unique_ptr<planner::LogicalPlan> getRoot(const std::string& query) {
        auto preparedStatement = conn->prepare(query);
        if (!preparedStatement->isSuccess()) {
            throw std::runtime_error("Failed to prepare optimizer test query:\n" + query +
                                     "\nError:\n" + preparedStatement->getErrorMessage());
        }
        auto cachedStatement =
            conn->getClientContext()->getCachedPreparedStatementManager().getCachedStatement(
                preparedStatement->getName());
        return std::move(cachedStatement->logicalPlan);
    }

    // Helper to check if a specific operator type exists in the plan
    static bool hasOperatorType(planner::LogicalOperator* op, planner::LogicalOperatorType type) {
        if (op->getOperatorType() == type) {
            return true;
        }
        for (auto i = 0u; i < op->getNumChildren(); ++i) {
            if (hasOperatorType(op->getChild(i).get(), type)) {
                return true;
            }
        }
        return false;
    }
};

class StatsOptimizerTest : public EmptyDBTest {
public:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }

    std::unique_ptr<planner::LogicalPlan> getRoot(const std::string& query) {
        auto preparedStatement = conn->prepare(query);
        if (!preparedStatement->isSuccess()) {
            throw std::runtime_error("Failed to prepare optimizer test query:\n" + query +
                                     "\nError:\n" + preparedStatement->getErrorMessage());
        }
        auto cachedStatement =
            conn->getClientContext()->getCachedPreparedStatementManager().getCachedStatement(
                preparedStatement->getName());
        return std::move(cachedStatement->logicalPlan);
    }

    static planner::LogicalFilter* getDeepestFilter(planner::LogicalOperator* op) {
        planner::LogicalFilter* result = nullptr;
        std::function<void(planner::LogicalOperator*)> visit;
        visit = [&](planner::LogicalOperator* current) {
            if (current->getOperatorType() == planner::LogicalOperatorType::FILTER) {
                result = current->ptrCast<planner::LogicalFilter>();
            }
            for (auto i = 0u; i < current->getNumChildren(); ++i) {
                visit(current->getChild(i).get());
            }
        };
        visit(op);
        return result;
    }
};

TEST_F(OptimizerTest, JoinHint) {
    // One hop
    auto q1 = "MATCH (a:person)-[e:knows]->(b:person) "
              "WHERE a.ID > 0 "
              "  AND e.date = date('1999-01-01') "
              "  AND b.ID > 2"
              "  AND a.ID + b.ID > 3 "
              "HINT a JOIN (e JOIN b) "
              "RETURN *";
    ASSERT_STREQ(getEncodedPlan(q1).c_str(),
        "Filter()HJ(a._ID){Filter()S(a)}{Filter()E(a)Filter()S(b)}");
    auto q2 = "MATCH (a:person)-[e:knows]->(b:person) "
              "WHERE a.ID > 0 "
              "  AND e.date = date('1999-01-01') "
              "  AND b.ID > 2"
              "  AND a.ID + b.ID > 3 "
              "HINT (a JOIN e) JOIN b "
              "RETURN *";
    ASSERT_STREQ(getEncodedPlan(q2).c_str(),
        "Filter()HJ(b._ID){Filter()E(b)Filter()S(a)}{Filter()S(b)}");
    // Two hop
    auto q3 = "MATCH (a:person)-[e1:knows]->(b:person)-[e2:knows]->(c:person) "
              "HINT (((b JOIN e1) JOIN e2) JOIN a) JOIN c "
              "RETURN *";
    ASSERT_STREQ(getEncodedPlan(q3).c_str(), "HJ(c._ID){HJ(a._ID){E(c)E(a)S(b)}{S(a)}}{S(c)}");
    auto q4 = "MATCH (a:person)-[e1:knows]->(b:person)-[e2:knows]->(c:person) "
              "HINT (((a JOIN e1) JOIN b) JOIN e2) JOIN c "
              "RETURN COUNT(*)";
    ASSERT_STREQ(getEncodedPlan(q4).c_str(), "HJ(b._ID){E(b)S(a)}{E(c)S(b)}");
    // Cycle
    auto q5 = "MATCH (a:person)-[e1:knows]->(b:person)-[e2:knows]->(a) "
              "HINT ((a JOIN e1) JOIN b) JOIN e2 "
              "RETURN *";
    ASSERT_STREQ(getEncodedPlan(q5).c_str(),
        "HJ(a._ID,b._ID){HJ(b._ID){E(b)S(a)}{S(b)}}{E(a)S(b)}");
    auto q6 = "MATCH (a:person)-[e1:knows]->(b:person)-[e2:knows]->(c:person), "
              "      (a)-[e3:knows]->(c) "
              "HINT (((a JOIN e1) JOIN b) MULTI_JOIN e2 MULTI_JOIN e3) JOIN c "
              "RETURN *";
    ASSERT_STREQ(getEncodedPlan(q6).c_str(),
        "HJ(c._ID){I(c._ID){HJ(b._ID){E(b)S(a)}{S(b)}}{E(c)S(b)}{E(c)S(a)}}{S(c)}");
}

TEST_F(OptimizerTest, CrossJoinWithFilterWithoutPushDownTest) {
    auto q1 = "MATCH (a:person) "
              "MATCH (b:person) "
              "WHERE a.fName=b.fName AND a.gender <> b.gender "
              "RETURN a.gender;";
    ASSERT_STREQ(getEncodedPlan(q1).c_str(), "Filter()HJ(a.fName=b.fName){S(a)}{S(b)}");
    auto q2 = "MATCH (a:person) "
              "MATCH (b:person) "
              "WHERE a.fName=b.fName AND a.fName is NOT null "
              "RETURN a.fName;";
    ASSERT_STREQ(getEncodedPlan(q2).c_str(), "HJ(a.fName=b.fName){Filter()S(a)}{S(b)}");
    auto q3 = "MATCH (a:person) "
              "MATCH (b:person) "
              "WHERE a.fName=b.fName AND a.age > 1 + 2.0 "
              "RETURN a.fName;";
    ASSERT_STREQ(getEncodedPlan(q3).c_str(), "HJ(a.fName=b.fName){Filter()S(a)}{S(b)}");
}

TEST_F(OptimizerTest, DisableOptimizerTest) {
    conn->query("CALL enable_plan_optimizer=false");

    auto q1 = "MATCH (a:person)-[e]->(b) "
              "WHERE a.ID < 0 AND a.fName='Alice' "
              "RETURN a.gender;";
    {
        ASSERT_STREQ(getEncodedPlan(q1).c_str(), "HJ(b._ID){S(b)}{E(b)Filter()Filter()S(a)}");
        // sanity check to see if the plan still outputs the correct result
        auto result = conn->query(q1);
        ASSERT_EQ(result->getNumTuples(), 0);
    }

    auto q2 = "MATCH (a:person)-[e:knows]->(b:person) "
              "HINT (a JOIN e) JOIN b "
              "RETURN e.date;";
    {
        ASSERT_STREQ(getEncodedPlan(q2).c_str(), "HJ(b._ID){E(b)S(a)}{S(b)}");
        auto result = conn->query(q2);
        ASSERT_EQ(result->getNumTuples(), 14);
    }

    conn->query("CALL enable_plan_optimizer=true");
    {
        ASSERT_STREQ(getEncodedPlan(q2).c_str(), "E(b)S(a)");
        auto result = conn->query(q2);
        ASSERT_EQ(result->getNumTuples(), 14);
    }
}

TEST_F(OptimizerTest, FilterPushDownTest) {
    auto q1 = "MATCH (a:person)-[e]->(b) "
              "WHERE a.ID < 0 AND a.fName='Alice' "
              "RETURN a.gender;";
    ASSERT_STREQ(getEncodedPlan(q1).c_str(), "E(b)Filter()Filter()S(a)");
}

TEST_F(OptimizerTest, IndexScanTest) {
    auto q1 = "MATCH (a:person) "
              "WHERE a.ID = 0 AND a.fName='Alice' "
              "RETURN a.gender;";
    ASSERT_STREQ(getEncodedPlan(q1).c_str(), "Filter()IndexScan(a)");
}

TEST_F(OptimizerTest, RemoveUnnecessaryJoinTest) {
    auto q1 = "MATCH (a:person)-[e:knows]->(b:person) "
              "HINT (a JOIN e) JOIN b "
              "RETURN e.date;";
    ASSERT_STREQ(getEncodedPlan(q1).c_str(), "E(b)S(a)");
}

TEST_F(OptimizerTest, MergeConsecutiveMatch) {
    auto q1 = "MATCH (a:person) MATCH (b:person) WHERE b.ID=0 MATCH (a)-[]->(b) "
              "RETURN COUNT(*);";
    if (common::DEFAULT_EXTEND_DIRECTION == common::ExtendDirection::FWD) {
        ASSERT_STREQ(getEncodedPlan(q1).c_str(), "HJ(b._ID){E(b)S(a)}{IndexScan(b)}");
    } else {
        ASSERT_STREQ(getEncodedPlan(q1).c_str(), "E(a)IndexScan(b)");
    }
}

TEST_F(OptimizerTest, PkScanTest) {
    auto q1 = "MATCH (a:person {ID:24189255811663})-[f]->(b) RETURN b;";
    auto ans = getEncodedPlan(q1);
    ASSERT_STREQ(ans.c_str(), "HJ(b._ID){S(b)}{E(b)IndexScan(a)}");
}

TEST_F(OptimizerTest, FilterDifferentPropertiesTest) {
    auto q1 = "MATCH (a:person {gender:1})-[f]->(b:person {age: 30}) RETURN b;";
    auto ans = getEncodedPlan(q1);
    ASSERT_TRUE(ans == "HJ(b._ID){E(b)Filter()S(a)}{Filter()S(b)}" ||
                ans == "HJ(a._ID){E(a)Filter()S(b)}{Filter()S(a)}");
}

TEST_F(OptimizerTest, SingleNodeTwoHopJoins) {
    if (common::DEFAULT_EXTEND_DIRECTION != common::ExtendDirection::BOTH) {
        GTEST_SKIP();
    }
#if defined(WIN32)
    // Skip on windows as we don't generate consistent plan as on other platforms.
    // TODO(Guodong/Xiyang): We should make sure the plan is consistent on all platforms.
    GTEST_SKIP();
#else
    auto q1 =
        "MATCH (a:person)-[e:knows]->(b:person)-[e2:knows]->(c:person) WHERE b.ID=0 RETURN a,b,c;";
    ASSERT_STREQ(getEncodedPlan(q1).c_str(),
        "HJ(c._ID){S(c)}{HJ(a._ID){S(a)}{E(c)E(a)IndexScan(b)}}");
    auto q2 = "MATCH (a:person)-[e:knows]->(b:person)-[e2:knows]->(c:person) WHERE a.ID=0 "
              "RETURN a,b,c;";
    ASSERT_STREQ(getEncodedPlan(q2).c_str(),
        "HJ(c._ID){S(c)}{HJ(b._ID){E(c)S(b)}{E(b)IndexScan(a)}}");
    auto q3 = "MATCH (a:person)-[e:knows]->(b:person)-[e2:knows]->(c:person) WHERE c.ID=0 "
              "RETURN a,b,c;";
    ASSERT_STREQ(getEncodedPlan(q3).c_str(),
        "HJ(a._ID){S(a)}{HJ(b._ID){E(a)S(b)}{E(b)IndexScan(c)}}");
#endif
}

TEST_F(OptimizerTest, PlanUndirectedInnerJoin) {
    if (common::DEFAULT_EXTEND_DIRECTION != common::ExtendDirection::BOTH) {
        GTEST_SKIP();
    }
    auto query = "MATCH (a:person)-[e:knows]-(b:person) RETURN a.ID, b.ID;";
    auto encodedPlan = getEncodedPlan(query);
    // there should only be a single hash join in the plan
    ASSERT_TRUE((encodedPlan == "HJ(b._ID){E(b)S(a)}{S(b)}") ||
                (encodedPlan == "HJ(a._ID){E(a)S(b)}{S(a)}"));
}

TEST_F(OptimizerTest, SubqueryHint) {
    auto q1 = "MATCH (a:person) WITH * MATCH (a)-[e:knows]->(b:person) WHERE b.ID > 0 HINT (a JOIN "
              "e) JOIN b RETURN *;";
    ASSERT_STREQ(getEncodedPlan(q1).c_str(), "HJ(a._ID){S(a)}{HJ(b._ID){E(b)S(a)}{Filter()S(b)}}");
    auto q2 = "MATCH (a:person) WITH * MATCH (a)-[e:knows]->(b:person) WHERE b.ID > 0 HINT a JOIN "
              "(e JOIN b)RETURN *;";
    ASSERT_STREQ(getEncodedPlan(q2).c_str(), "HJ(a._ID){S(a)}{E(a)Filter()S(b)}");
    auto q3 = "MATCH (a:person) WITH * OPTIONAL MATCH (a)-[e:knows]->(b:person) WHERE b.ID > 0 "
              "HINT (a JOIN e) JOIN b RETURN *;";
    ASSERT_STREQ(getEncodedPlan(q3).c_str(), "HJ(a._ID){S(a)}{HJ(b._ID){E(b)S(a)}{Filter()S(b)}}");
    auto q4 = "MATCH (a:person) WITH * OPTIONAL MATCH (a)-[e:knows]->(b:person) WHERE b.ID > 0 "
              "HINT a JOIN (e JOIN b) RETURN *;";
    ASSERT_STREQ(getEncodedPlan(q4).c_str(), "HJ(a._ID){S(a)}{E(a)Filter()S(b)}");
    auto q5 = "MATCH (a:person) WHERE EXISTS { MATCH (a)-[e:knows]->(b:person) WHERE b.ID > 0 HINT "
              "(a JOIN e) JOIN b } RETURN *;";
    ASSERT_STREQ(getEncodedPlan(q5).c_str(),
        "Filter()HJ(a._ID){S(a)}{HJ(b._ID){E(b)S(a)}{Filter()S(b)}}");
    auto q6 = "MATCH (a:person) WHERE EXISTS { MATCH (a)-[e:knows]->(b:person) WHERE b.ID > 0 HINT "
              "a JOIN (e JOIN b) } RETURN *;";
    ASSERT_STREQ(getEncodedPlan(q6).c_str(), "Filter()HJ(a._ID){S(a)}{E(a)Filter()S(b)}");
}

TEST_F(OptimizerTest, CountRelTableOptimizer) {
#if defined(_WIN32)
    GTEST_SKIP() << "Windows can pick the reverse physical extend orientation for degree-count "
                    "plans, so this plan-shape test is nondeterministic there.";
#endif

    ASSERT_TRUE(conn->query("CREATE NODE TABLE opt_user(id INT64, PRIMARY KEY(id));")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE REL TABLE opt_follows(FROM opt_user TO opt_user, date DATE);")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:opt_user {id: 1});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:opt_user {id: 2});")->isSuccess());
    ASSERT_TRUE(conn->query("MATCH (a:opt_user), (b:opt_user) "
                            "WHERE a.id = 1 AND b.id = 2 "
                            "CREATE (a)-[:opt_follows {date: date('2020-01-01')}]->(b);")
                    ->isSuccess());

    // Test that COUNT(*) over a single rel table is optimized to COUNT_REL_TABLE
    auto q1 = "MATCH (a:opt_user)-[e:opt_follows]->(b:opt_user) RETURN COUNT(*);";
    auto plan1 = getRoot(q1);
    ASSERT_TRUE(hasOperatorType(plan1->getLastOperator().get(),
        planner::LogicalOperatorType::COUNT_REL_TABLE));
    // Verify the query returns the correct result
    auto result1 = conn->query(q1);
    ASSERT_TRUE(result1->isSuccess());
    ASSERT_EQ(result1->getNumTuples(), 1);
    auto tuple1 = result1->getNext();
    ASSERT_EQ(tuple1->getValue(0)->getValue<int64_t>(), 1);

    // Test that COUNT(*) with GROUP BY is NOT optimized (has keys)
    auto q2 = "MATCH (a:opt_user)-[e:opt_follows]->(b:opt_user) RETURN a.id, COUNT(*);";
    auto plan2 = getRoot(q2);
    ASSERT_FALSE(hasOperatorType(plan2->getLastOperator().get(),
        planner::LogicalOperatorType::COUNT_REL_TABLE));

    // Test that COUNT(*) with WHERE clause is NOT optimized (has filter)
    auto q3 = "MATCH (a:opt_user)-[e:opt_follows]->(b:opt_user) WHERE a.id > 0 RETURN COUNT(*);";
    auto plan3 = getRoot(q3);
    ASSERT_FALSE(hasOperatorType(plan3->getLastOperator().get(),
        planner::LogicalOperatorType::COUNT_REL_TABLE));

    // Test that COUNT(DISTINCT ...) is NOT optimized
    auto q4 = "MATCH (a:opt_user)-[e:opt_follows]->(b:opt_user) RETURN COUNT(DISTINCT a);";
    auto plan4 = getRoot(q4);
    ASSERT_FALSE(hasOperatorType(plan4->getLastOperator().get(),
        planner::LogicalOperatorType::COUNT_REL_TABLE));

    // Test that COUNT(rel) over a full unfiltered rel scan is optimized to COUNT_REL_TABLE
    auto q5 = "MATCH (a:opt_user)-[e:opt_follows]->(b:opt_user) RETURN COUNT(e);";
    auto plan5 = getRoot(q5);
    ASSERT_TRUE(hasOperatorType(plan5->getLastOperator().get(),
        planner::LogicalOperatorType::COUNT_REL_TABLE));
    auto result5 = conn->query(q5);
    ASSERT_TRUE(result5->isSuccess());
    ASSERT_EQ(result5->getNumTuples(), 1);
    auto tuple5 = result5->getNext();
    ASSERT_EQ(tuple5->getValue(0)->getValue<int64_t>(), 1);

    // Test that COUNT(node) is NOT optimized by the rel-table fast path
    auto q6 = "MATCH (a:opt_user)-[e:opt_follows]->(b:opt_user) RETURN COUNT(a);";
    auto plan6 = getRoot(q6);
    ASSERT_FALSE(hasOperatorType(plan6->getLastOperator().get(),
        planner::LogicalOperatorType::COUNT_REL_TABLE));

    // Test that COUNT(rel property) is NOT optimized
    auto q7 = "MATCH (a:opt_user)-[e:opt_follows]->(b:opt_user) RETURN COUNT(e.date);";
    auto plan7 = getRoot(q7);
    ASSERT_FALSE(hasOperatorType(plan7->getLastOperator().get(),
        planner::LogicalOperatorType::COUNT_REL_TABLE));

    // Test active-write degree queries over native rel tables.
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE opt_degree_user(id INT64, PRIMARY KEY(id));")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE REL TABLE opt_degree_follows(FROM opt_degree_user TO "
                            "opt_degree_user);")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:opt_degree_user {id: 0});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:opt_degree_user {id: 1});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:opt_degree_user {id: 2});")->isSuccess());
    ASSERT_TRUE(conn->query("MATCH (a:opt_degree_user), (b:opt_degree_user) "
                            "WHERE a.id = 0 AND b.id = 1 "
                            "CREATE (a)-[:opt_degree_follows]->(b);")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("MATCH (a:opt_degree_user), (b:opt_degree_user) "
                            "WHERE a.id = 0 AND b.id = 2 "
                            "CREATE (a)-[:opt_degree_follows]->(b);")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("MATCH (a:opt_degree_user), (b:opt_degree_user) "
                            "WHERE a.id = 1 AND b.id = 2 "
                            "CREATE (a)-[:opt_degree_follows]->(b);")
                    ->isSuccess());

    auto q8 = "MATCH (u:opt_degree_user)-[:opt_degree_follows]->(v) RETURN count(DISTINCT u.id);";
    auto plan8 = getRoot(q8);
    ASSERT_TRUE(hasOperatorType(plan8->getLastOperator().get(),
        planner::LogicalOperatorType::REL_DEGREE_TABLE));
    auto result8 = conn->query(q8);
    ASSERT_TRUE(result8->isSuccess());
    ASSERT_EQ(result8->getNext()->getValue(0)->getValue<int64_t>(), 2);

    auto q9 = "MATCH (u:opt_degree_user)-[:opt_degree_follows]->(v) "
              "RETURN u.id, count(v) AS deg ORDER BY deg DESC LIMIT 2;";
    auto plan9 = getRoot(q9);
    ASSERT_TRUE(hasOperatorType(plan9->getLastOperator().get(),
        planner::LogicalOperatorType::REL_DEGREE_TABLE));
    auto result9 = conn->query(q9);
    ASSERT_TRUE(result9->isSuccess());
    auto tuple9a = result9->getNext();
    ASSERT_EQ(tuple9a->getValue(0)->getValue<int64_t>(), 0);
    ASSERT_EQ(tuple9a->getValue(1)->getValue<int64_t>(), 2);
    auto tuple9b = result9->getNext();
    ASSERT_EQ(tuple9b->getValue(0)->getValue<int64_t>(), 1);
    ASSERT_EQ(tuple9b->getValue(1)->getValue<int64_t>(), 1);

    auto q10 =
        "MATCH (a:opt_degree_user)<-[e:opt_degree_follows]-(b:opt_degree_user) RETURN COUNT(e);";
    auto plan10 = getRoot(q10);
    ASSERT_TRUE(hasOperatorType(plan10->getLastOperator().get(),
        planner::LogicalOperatorType::COUNT_REL_TABLE));
    auto result10 = conn->query(q10);
    ASSERT_TRUE(result10->isSuccess());
    ASSERT_EQ(result10->getNext()->getValue(0)->getValue<int64_t>(), 3);

    // Test that degree top-k and active bound count merge duplicate bound nodes across rel tables.
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE opt_multi_user(id INT64, PRIMARY KEY(id));")->isSuccess());
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE opt_multi_target(id INT64, PRIMARY KEY(id));")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE REL TABLE opt_multi(FROM opt_multi_user TO opt_multi_user, "
                            "FROM opt_multi_user TO opt_multi_target);")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:opt_multi_user {id: 0});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:opt_multi_user {id: 1});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:opt_multi_target {id: 0});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:opt_multi_target {id: 1});")->isSuccess());
    ASSERT_TRUE(conn->query("MATCH (a:opt_multi_user), (b:opt_multi_user) "
                            "WHERE a.id = 0 AND b.id = 1 CREATE (a)-[:opt_multi]->(b);")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("MATCH (a:opt_multi_user), (b:opt_multi_target) "
                            "WHERE a.id = 0 AND b.id = 0 CREATE (a)-[:opt_multi]->(b);")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("MATCH (a:opt_multi_user), (b:opt_multi_target) "
                            "WHERE a.id = 1 AND b.id = 1 CREATE (a)-[:opt_multi]->(b);")
                    ->isSuccess());
    auto q11 = "MATCH (u:opt_multi_user)-[:opt_multi]->(v) "
               "RETURN u.id, count(*) AS deg ORDER BY deg DESC LIMIT 2;";
    auto plan11 = getRoot(q11);
    ASSERT_TRUE(hasOperatorType(plan11->getLastOperator().get(),
        planner::LogicalOperatorType::REL_DEGREE_TABLE));
    auto result11 = conn->query(q11);
    ASSERT_TRUE(result11->isSuccess());
    auto tuple11a = result11->getNext();
    ASSERT_EQ(tuple11a->getValue(0)->getValue<int64_t>(), 0);
    ASSERT_EQ(tuple11a->getValue(1)->getValue<int64_t>(), 2);
    auto tuple11b = result11->getNext();
    ASSERT_EQ(tuple11b->getValue(0)->getValue<int64_t>(), 1);
    ASSERT_EQ(tuple11b->getValue(1)->getValue<int64_t>(), 1);

    auto q12 = "MATCH (u:opt_multi_user)-[:opt_multi]->(v) RETURN count(DISTINCT u.id);";
    auto plan12 = getRoot(q12);
    ASSERT_TRUE(hasOperatorType(plan12->getLastOperator().get(),
        planner::LogicalOperatorType::REL_DEGREE_TABLE));
    auto result12 = conn->query(q12);
    ASSERT_TRUE(result12->isSuccess());
    ASSERT_EQ(result12->getNext()->getValue(0)->getValue<int64_t>(), 2);
}

TEST_F(StatsOptimizerTest, FilterPushDownOrdersMostSelectivePredicateFirst) {
    ASSERT_TRUE(conn->query("CREATE NODE TABLE stats_node(id INT64, common INT64, rare INT64, "
                            "PRIMARY KEY(id));")
                    ->isSuccess());
    for (auto i = 0; i < 20; ++i) {
        auto result = conn->query(
            std::format("CREATE (:stats_node {{id: {}, common: {}, rare: {}}});", i, i % 2, i));
        ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    }
    ASSERT_TRUE(conn->query("ANALYZE stats_node;")->isSuccess());

    auto plan = getRoot("EXPLAIN LOGICAL MATCH (n:stats_node) "
                        "WHERE n.common = 1 AND n.rare = 7 "
                        "RETURN n.id;");
    auto* deepestFilter = getDeepestFilter(plan->getLastOperator().get());
    ASSERT_NE(nullptr, deepestFilter);
    ASSERT_NE(std::string::npos, deepestFilter->getPredicate()->toString().find("rare"));
}

TEST_F(StatsOptimizerTest, FilterPushDownOrdersMostSelectivePredicateFirst2) {
    // This test is similar to the previous one, but uses UNWIND instead of a loop
    ASSERT_TRUE(conn->query("CREATE NODE TABLE stats_node(id INT64, common INT64, rare INT64, "
                            "PRIMARY KEY(id));")
                    ->isSuccess());
    auto result = conn->query("UNWIND range(0, 19) AS i "
                              "CREATE (:stats_node {id: i, common: i % 2, rare: i});");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(conn->query("ANALYZE stats_node;")->isSuccess());

    auto plan = getRoot("EXPLAIN LOGICAL MATCH (n:stats_node) "
                        "WHERE n.common = 1 AND n.rare = 7 "
                        "RETURN n.id;");
    auto* deepestFilter = getDeepestFilter(plan->getLastOperator().get());
    ASSERT_NE(nullptr, deepestFilter);
    ASSERT_NE(std::string::npos, deepestFilter->getPredicate()->toString().find("rare"));
}

TEST_F(StatsOptimizerTest, HashJoinCostIncludesEstimatedOutputCardinality) {
    auto leftOp = std::make_shared<planner::LogicalDummyScan>();
    leftOp->setCardinality(10);
    leftOp->computeFlatSchema();
    auto rightOp = std::make_shared<planner::LogicalDummyScan>();
    rightOp->setCardinality(10);
    rightOp->computeFlatSchema();

    auto leftPlan = planner::LogicalPlan();
    leftPlan.setLastOperator(leftOp);
    auto rightPlan = planner::LogicalPlan();
    rightPlan.setLastOperator(rightOp);

    auto joinKeys = binder::expression_vector{};
    const auto smallIntermediateCost =
        planner::CostModel::computeHashJoinCost(joinKeys, leftPlan, rightPlan, 5);
    const auto largeIntermediateCost =
        planner::CostModel::computeHashJoinCost(joinKeys, leftPlan, rightPlan, 500);
    ASSERT_LT(smallIntermediateCost, largeIntermediateCost);
}

} // namespace testing
} // namespace lbug

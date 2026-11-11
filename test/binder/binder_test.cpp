#include <algorithm>

#include "binder/binder.h"
#include "binder/expression/property_expression.h"
#include "binder/query/bound_regular_query.h"
#include "binder/visitor/confidential_statement_analyzer.h"
#include "graph_test/private_graph_test.h"
#include "parser/parser.h"

namespace lbug {
namespace testing {

TEST_F(EmptyDBTest, ConfidentialStatementVisitor) {
    createDBAndConn();
    conn->query("BEGIN TRANSACTION");
    database->addExtensionOption("binder_test_conf", common::LogicalTypeID::UINT64,
        common::Value{5}, true /* isConfidential */);
    database->addExtensionOption("binder_test_not_conf", common::LogicalTypeID::UINT64,
        common::Value{2}, false /* isConfidential */);
    binder::Binder binder{conn->getClientContext()};

    binder::ConfidentialStatementAnalyzer analyzerConfidential{};
    auto parsed = parser::Parser::parseQuery("CALL binder_test_conf = 3");
    auto boundStatement = binder.bind(*parsed[0]);
    analyzerConfidential.visit(*boundStatement);
    ASSERT_TRUE(analyzerConfidential.isConfidential());

    binder::ConfidentialStatementAnalyzer analyzerNotConfidential{};
    parsed = parser::Parser::parseQuery("CALL binder_test_not_conf = 444");
    boundStatement = binder.bind(*parsed[0]);
    analyzerConfidential.visit(*boundStatement);
    ASSERT_FALSE(analyzerConfidential.isConfidential());

    parsed = parser::Parser::parseQuery("CALL timeout = 4");
    boundStatement = binder.bind(*parsed[0]);
    analyzerNotConfidential.visit(*boundStatement);
    ASSERT_FALSE(analyzerNotConfidential.isConfidential());
}

TEST_F(EmptyDBTest, LargeConstantListIsNotFolded) {
    createDBAndConn();
    conn->query("BEGIN TRANSACTION");
    auto bindReturnType = [&](const std::string& expression) {
        binder::Binder binder{conn->getClientContext()};
        auto parsed = parser::Parser::parseQuery("RETURN " + expression);
        return binder.bind(*parsed[0])->getStatementResult()->getColumns()[0]->expressionType;
    };
    const auto literal = common::ExpressionType::LITERAL;
    const auto function = common::ExpressionType::FUNCTION;
    EXPECT_EQ(bindReturnType("range(1, 8192)"), literal);
    EXPECT_EQ(bindReturnType("range(1, 8193)"), function);
    EXPECT_EQ(bindReturnType("[range(1, 9000)]"), function);
    EXPECT_EQ(bindReturnType("{a: range(1, 9000)}"), function);
    // Built from folded parts, or written out in the query: no larger than the expression.
    EXPECT_EQ(bindReturnType("[range(1, 5000), range(1, 5000)]"), literal);
    std::string writtenOutList = "[0";
    for (auto i = 1; i < 10000; i++) {
        writtenOutList += ", " + std::to_string(i);
    }
    writtenOutList += "]";
    EXPECT_EQ(bindReturnType(writtenOutList), literal);
    EXPECT_EQ(bindReturnType("CASE WHEN true THEN " + writtenOutList + " ELSE [1] END"), literal);
}

// The late property fetches the rewriter records for the last query part, as sorted
// "variable.property" names followed by how the planner reads them.
static std::string getLateFetches(main::ClientContext* context, const std::string& query) {
    binder::Binder binder{context};
    auto parsed = parser::Parser::parseQuery(query);
    auto boundStatement = binder.bind(*parsed[0]);
    auto singleQuery = boundStatement->constCast<binder::BoundRegularQuery>().getSingleQuery(0);
    auto lastPart = singleQuery->getQueryPart(singleQuery->getNumQueryParts() - 1);
    std::vector<std::string> fetches;
    for (auto& fetch : lastPart->getLatePropertyFetches()) {
        std::vector<std::string> names;
        for (auto& expr : fetch.properties) {
            auto& property = expr->constCast<binder::PropertyExpression>();
            names.push_back(property.getRawVariableName() + "." + property.getPropertyName());
        }
        std::sort(names.begin(), names.end());
        std::string text;
        for (auto& name : names) {
            text += name + " ";
        }
        fetches.push_back(text + (fetch.useLookup ? "lookup;" : "join;"));
    }
    std::sort(fetches.begin(), fetches.end());
    std::string result;
    for (auto& fetch : fetches) {
        result += fetch;
    }
    return result;
}

TEST_F(EmptyDBTest, LatePropertyFetchConditions) {
    createDBAndConn();
    ASSERT_TRUE(conn->query("CREATE NODE TABLE P(id INT64 PRIMARY KEY, name STRING, age INT64)")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("CREATE NODE TABLE Q(id INT64 PRIMARY KEY, name STRING)")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE REL TABLE K(FROM P TO P, FROM P TO Q)")->isSuccess());
    ASSERT_TRUE(conn->query("BEGIN TRANSACTION")->isSuccess());
    auto context = conn->getClientContext();
    EXPECT_EQ(getLateFetches(context, "MATCH (a:P)-[:K]->(b:P) MATCH (b)-[:K]->(d:Q) "
                                      "WITH b, count(*) AS c RETURN b.name, c ORDER BY b.id"),
        "b.id b.name join;");
    EXPECT_EQ(getLateFetches(context,
                  "MATCH (a:P)-[:K]->(b:P) MATCH (b)-[:K]->(d:Q) WITH DISTINCT b RETURN b.name"),
        "b.name join;");
    EXPECT_EQ(getLateFetches(context,
                  "MATCH (a:P)-[:K]->(b:P) UNWIND [1, 2] AS x WITH b LIMIT 2000 RETURN b.name"),
        "b.name join;");
    EXPECT_EQ(getLateFetches(context, "MATCH (a:P)-[:K]->(b:P) WHERE b.age > 1 "
                                      "MATCH (b)-[:K]->(d:Q) WITH b, count(*) AS c "
                                      "RETURN b.name, b.age, c"),
        "b.name join;");
    EXPECT_EQ(getLateFetches(context, "MATCH (a:P)-[:K]->(b:P) MATCH (b)-[:K]->(d:Q) "
                                      "WITH b, count(*) AS c RETURN b, c"),
        "b.age b.id b.name join;");
    EXPECT_EQ(getLateFetches(context,
                  "MATCH (a:P)-[:K]->(b:P) WITH b, count(*) AS c "
                  "MATCH (b)-[:K]->(d:Q) WITH b, count(d) AS e RETURN b.name, e"),
        "b.name join;");
    EXPECT_EQ(getLateFetches(context, "MATCH (a:P)-[:K]->(b:P) WITH DISTINCT a, b "
                                      "WITH b, count(a) AS c RETURN b.name, c"),
        "b.name join;");
    EXPECT_EQ(
        getLateFetches(context, "MATCH (a:P) MATCH (a)-[:K]->(d:Q) WITH a LIMIT $k RETURN a.name"),
        "a.name join;");
    EXPECT_EQ(
        getLateFetches(context, "MATCH (a:P) MATCH (a)-[:K]->(d:Q) WITH a SKIP 1 RETURN a.name"),
        "a.name join;");
    EXPECT_EQ(getLateFetches(context, "MATCH (a:P) MATCH (a)-[:K]->(d:Q) WITH a ORDER BY a.age "
                                      "LIMIT 1025 RETURN a.name ORDER BY a.name"),
        "a.name join;");
    EXPECT_EQ(getLateFetches(context, "MATCH (a:P)-[:K]->(b:P) MATCH (b)-[:K]->(d:Q) "
                                      "WITH b, count(*) AS c RETURN id(b), b.name"),
        "b.name join;");
    EXPECT_EQ(getLateFetches(context, "MATCH (a:P) WITH a ORDER BY a.age LIMIT 1024 RETURN a.name"),
        "a.name lookup;");
    EXPECT_EQ(getLateFetches(context,
                  "MATCH (a:P)-[:K]->(b:P) WITH a, b, count(*) AS c LIMIT 5 RETURN a.name, b.age"),
        "a.name lookup;b.age lookup;");
    // Recorded before label pruning, which leaves b without a table (K has no FROM Q); the planner
    // then uses the join (OptimizerTest.LatePropertiesOfPrunedNodeUseJoin).
    EXPECT_EQ(getLateFetches(context, "MATCH (b:Q)-[:K]->(c:P) WITH b LIMIT 5 RETURN b.name"),
        "b.name lookup;");
    // b is narrowed to Q by the second pattern and keeps P's age, which is NULL.
    EXPECT_EQ(getLateFetches(context, "MATCH (b) MATCH (b:Q) WITH b LIMIT 5 RETURN b.name, b.age"),
        "b.name lookup;");
    EXPECT_EQ(getLateFetches(context, "MATCH (a:P)-[:K]->(b:P) MATCH (b)-[:K]->(d:Q) "
                                      "WITH b AS x, count(*) AS c RETURN x.name, c"),
        "b.name join;");
    // Each of these breaks one condition.
    for (auto& query : std::vector<std::string>{
             "MATCH (a:P) RETURN a.name",
             "MATCH (a:P)-[:K]->(b:P) WITH b, count(*) AS c RETURN b.name, c",
             "MATCH (a:P)-[:K]->(b:P) MATCH (b)-[:K]->(d:Q) WITH d, count(*) AS c RETURN d.name, c",
             "MATCH (a:P) OPTIONAL MATCH (a)-[:K]->(b:P) MATCH (a)-[:K]->(d:Q) "
             "WITH b, count(*) AS c RETURN b.name, c",
             "MATCH (a:P)-[:K]->(b) MATCH (a)-[:K]->(d:Q) WITH b, count(*) AS c RETURN b.name, c",
             "MATCH (a:P) MATCH (a)-[:K]->(d:Q) WITH a ORDER BY a.age LIMIT 1025 RETURN a.name",
             "MATCH (a:P) MATCH (a)-[:K]->(d:Q) WITH a ORDER BY a.age LIMIT $k RETURN a.name",
             "MATCH (a:P) MATCH (a)-[:K]->(d:Q) WITH a ORDER BY a.age SKIP 1 RETURN a.name",
             "MATCH (a:P) WITH a ORDER BY a.age LIMIT 5000 MATCH (a)-[:K]->(d:Q) "
             "WITH a LIMIT 2000 RETURN a.name",
             "MATCH (a:P)-[:K]->(b:P) MATCH (b)-[:K]->(d:Q) WITH b, a.age AS x RETURN b.name, x",
             "MATCH (a:P)-[:K]->(b:P) MATCH (b)-[:K]->(d:Q) SET b.age = 1 WITH b, count(*) AS c "
             "RETURN b.name, c",
             "MATCH (a:P)-[:K]->(b:P) MATCH (b)-[:K]->(d:Q) WITH b, count(*) AS c "
             "MATCH (b)-[:K]->(e:Q) RETURN b.name, e.name",
             "MATCH (a:P)-[:K]->(b:P) MATCH (b)-[:K]->(d:Q) WITH b, count(*) AS c "
             "RETURN b.name, EXISTS { MATCH (b)-[:K]->(:Q) }",
             "MATCH (a:P)-[:K]->(b:P) MATCH (b)-[:K]->(d:Q) WITH b, count(*) AS c "
             "RETURN b.name ORDER BY EXISTS { MATCH (x:Q) WHERE x.id = 1 }",
             // The RETURN's aggregate reads rows in the earlier ORDER BY's order.
             "MATCH (a:P) MATCH (a)-[:K]->(d:Q) WITH a ORDER BY a.age LIMIT 1025 "
             "RETURN a.id, collect(a.name) ORDER BY a.id",
             // One pattern: no reading clause follows the one that binds b.
             "MATCH (a:P)-[:K]->(b:P)-[:K]->(d:Q) WITH b, count(*) AS c RETURN b.name, c",
         }) {
        EXPECT_EQ(getLateFetches(context, query), "") << query;
    }
    ASSERT_TRUE(conn->query("CALL enable_plan_optimizer=false")->isSuccess());
    EXPECT_EQ(getLateFetches(context, "MATCH (a:P) WITH a ORDER BY a.age LIMIT 5 RETURN a.name"),
        "");
    ASSERT_TRUE(conn->query("CALL enable_plan_optimizer=true")->isSuccess());
    ASSERT_TRUE(conn->query("COMMIT")->isSuccess());
}

} // namespace testing
} // namespace lbug

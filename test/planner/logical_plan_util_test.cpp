#include "graph_test/private_graph_test.h"
#include "planner/operator/logical_plan_util.h"
#include "test_runner/test_runner.h"

using namespace lbug::planner;
using namespace lbug::testing;

class LogicalPlanUtilTest : public DBTest {
public:
    std::string getInputDir() override {
        return TestHelper::appendLbugRootPath("dataset/tinysnb/");
    }

    bool hasOrderBy(const std::string& query) {
        auto plan = TestRunner::getLogicalPlan(query, *conn);
        return LogicalPlanUtil::hasOrderByOnDataPath(*plan->getLastOperator());
    }
};

// --- Positive: ORDER BY is on the data-flow path, so the expensive
// deterministic CSR metadata merge is required. ---
TEST_F(LogicalPlanUtilTest, SimpleOrderByIsOnDataPath) {
    EXPECT_TRUE(hasOrderBy("MATCH (a:person) RETURN a ORDER BY a.rowid"));
}

TEST_F(LogicalPlanUtilTest, OrderByUnderLimitIsOnDataPath) {
    EXPECT_TRUE(hasOrderBy("MATCH (a:person) RETURN a ORDER BY a.rowid LIMIT 5"));
}

TEST_F(LogicalPlanUtilTest, OrderByUnderFilterIsOnDataPath) {
    EXPECT_TRUE(hasOrderBy("MATCH (a:person) WHERE a.age > 18 RETURN a ORDER BY a.rowid"));
}

TEST_F(LogicalPlanUtilTest, OrderByUnderProjectionIsOnDataPath) {
    // RETURN clause produces a projection; ORDER BY is its child on the data path.
    EXPECT_TRUE(hasOrderBy("MATCH (a:person)-[:knows]->(b:person) "
                           "RETURN a.fName, b.fName ORDER BY a.rowid"));
}

// --- Negative: no effective ORDER BY on the data path, so the expensive
// merge must be skipped. ---
TEST_F(LogicalPlanUtilTest, NoOrderByReturnsFalse) {
    EXPECT_FALSE(hasOrderBy("MATCH (a:person) RETURN a"));
}

TEST_F(LogicalPlanUtilTest, JoinWithoutOrderByReturnsFalse) {
    EXPECT_FALSE(hasOrderBy("MATCH (a:person)-[r:knows]->(b:person) RETURN a, b"));
}

TEST_F(LogicalPlanUtilTest, UnionWithoutOrderByReturnsFalse) {
    // UNION combines independent subplans; with no outer ORDER BY the
    // combined result is unordered, so the walker must stop at the UNION.
    EXPECT_FALSE(
        hasOrderBy("MATCH (a:person) RETURN a.fName UNION MATCH (b:person) RETURN b.fName"));
}

TEST_F(LogicalPlanUtilTest, RecursiveExtendWithoutOrderByReturnsFalse) {
    // Recursive traversal starts a new subplan; with no outer ORDER BY the
    // result is unordered, so the walker must stop at RECURSIVE_EXTEND.
    EXPECT_FALSE(hasOrderBy("MATCH (a:person)-[:knows*1..2]->(b:person) RETURN a.rowid, b.rowid"));
}

TEST_F(LogicalPlanUtilTest, AggregateWithoutOrderByReturnsFalse) {
    EXPECT_FALSE(hasOrderBy("MATCH (a:person) RETURN count(*)"));
}

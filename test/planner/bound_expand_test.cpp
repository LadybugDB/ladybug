#include <filesystem>
#include <string>
#include <vector>

#include "graph_test/private_graph_test.h"
#include "planner/operator/extend/logical_extend.h"
#include "planner/operator/logical_filter.h"
#include "planner/operator/logical_operator.h"
#include "planner/planner.h"
#include "test_helper/test_helper.h"
#include "test_runner/test_runner.h"

namespace lbug {
namespace testing {

// Regression tests for the bound-expand review (generalized lookup join for
// adjacency, see plan_subquery.cpp): the DISTINCT/AGGREGATE outer FLATTEN must
// survive the optimizer, parameterized leg predicates must still push into the
// property-attach scan, untrusted outers must fall back, and an ORDER BY only
// forces the fallback when it reaches the extend on an order-preserving path.
class BoundExpandTest : public DBTest {
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

    static bool hasBoundFlattenExtend(planner::LogicalOperator* root) {
        std::vector<planner::LogicalOperator*> extends;
        collectOps(root, planner::LogicalOperatorType::EXTEND, extends);
        for (auto* op : extends) {
            if (op->constCast<planner::LogicalExtend>().requireBoundFlattenFlag()) {
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

TEST_F(BoundExpandTest, DistinctLimitKeepsFlatten) {
    // DISTINCT outer is factorized; bound expand needs its pre-extend FLATTEN to
    // survive Remove/Factorization rewriters (opt-in LogicalExtend flag).
    auto plan = getRoot("MATCH (a:person)-[:knows]->(x:person) WITH DISTINCT a LIMIT 1000 "
                        "MATCH (a)-[:knows]->(b:person) RETURN count(*)");
    EXPECT_TRUE(hasBoundFlattenExtend(plan->getLastOperator().get()));
    EXPECT_TRUE(hasOp(plan->getLastOperator().get(), planner::LogicalOperatorType::FLATTEN));
}

TEST_F(BoundExpandTest, GroupByWithoutLimitFallsBack) {
    // The outer's AGGREGATE is not on the credibility allowlist, so the trust check
    // fails closed before the cost comparison is even reached: the GROUP BY outer
    // stays on the hash-join path.
    auto plan = getRoot("MATCH (a:person)-[:knows]->(x:person) WITH a, count(*) AS c "
                        "MATCH (a)-[:knows]->(b:person) RETURN count(*)");
    EXPECT_FALSE(hasBoundFlattenExtend(plan->getLastOperator().get()));
}

TEST_F(BoundExpandTest, CostComparisonFiresOnSmallTrustedOuter) {
    // No LIMIT and a trusted outer (PK seed plus one extend, depth 1): the cost
    // comparison decides. ANALYZE provides the CSR degree stats the worst-case
    // fan-out needs; without them the leg check fails closed. Three probes times
    // the studyAt fan-out stays far below the eliminated organisation scan, so
    // bound expand fires. (A bare `MATCH (a:person {ID: 0}) WITH a` would merge
    // with the leg before planning and never reach this path.)
    ASSERT_TRUE(conn->query("ANALYZE;")->isSuccess());
    auto plan = getRoot("MATCH (a:person {ID: 0})-[:knows]->(f:person) WITH f "
                        "MATCH (f)-[:studyAt]->(o:organisation) RETURN count(*)");
    EXPECT_TRUE(hasBoundFlattenExtend(plan->getLastOperator().get()));
}

TEST_F(BoundExpandTest, CostComparisonDeclinesOnLargeOuter) {
    // Same shape as above but an 8-row outer: the worst-case fan-out (8 probes
    // times the knows rate alone already exceeds the 8-row person scan) loses the
    // cost comparison, so the outer falls back to the hash-join path. Trust passes
    // here (plain scan outer), isolating the cost decision from the trust check.
    ASSERT_TRUE(conn->query("ANALYZE;")->isSuccess());
    auto plan = getRoot("MATCH (a:person) WITH a "
                        "MATCH (a)-[:knows]->(b:person) RETURN count(*)");
    EXPECT_FALSE(hasBoundFlattenExtend(plan->getLastOperator().get()));
}

TEST_F(BoundExpandTest, OrderByOnPreservingPathFallsBack) {
    // Outer ORDER BY reaches the extend through LIMIT only: the correlated extend
    // does not preserve it for downstream LIMIT, so fall back.
    auto plan = getRoot("MATCH (a:person) WITH a ORDER BY a.ID LIMIT 3 "
                        "MATCH (a)-[:knows]->(b:person) RETURN a.ID, b.ID");
    EXPECT_FALSE(hasBoundFlattenExtend(plan->getLastOperator().get()));
}

TEST_F(BoundExpandTest, OrderByUnderAggregateDoesNotBlock) {
    // The ORDER BY sits below the GROUP BY aggregate, where the order is already
    // gone; with a LIMIT the bound-expand path is still safe.
    auto plan = getRoot("MATCH (a:person)-[:knows]->(x:person) WITH a ORDER BY a.ID LIMIT 1000 "
                        "WITH a, count(*) AS c LIMIT 1000 "
                        "MATCH (a)-[:knows]->(b:person) RETURN count(*)");
    EXPECT_TRUE(hasBoundFlattenExtend(plan->getLastOperator().get()));
}

TEST_F(BoundExpandTest, ParameterPredicateStillPushes) {
    // $parameter is not Schema::evaluable, but a predicate depending only on the
    // attach target must still be pushed into its scan and must not block the path.
    // Prepare with the parameter bound so the binder resolves its type; the plan is
    // cached from this prepare, mirroring execution with bound parameters.
    std::unordered_map<std::string, std::unique_ptr<common::Value>> params;
    params["maxAge"] = std::make_unique<common::Value>((int64_t)30);
    auto prepared = conn->prepareWithParams(
        "MATCH (a:person) WITH a LIMIT 3 "
        "MATCH (a)-[:knows]->(b:person) WHERE b.age < $maxAge RETURN count(*)",
        std::move(params));
    ASSERT_TRUE(prepared->isSuccess()) << prepared->getErrorMessage();
    auto cached = conn->getClientContext()->getCachedPreparedStatementManager().getCachedStatement(
        prepared->getName());
    auto* root = cached->logicalPlan->getLastOperator().get();
    EXPECT_TRUE(hasBoundFlattenExtend(root));
    // The age filter is pushed below the attach hash join, onto the node scan.
    std::vector<planner::LogicalOperator*> filters;
    collectOps(root, planner::LogicalOperatorType::FILTER, filters);
    auto pushed = false;
    for (auto* op : filters) {
        auto& filter = op->constCast<planner::LogicalFilter>();
        if (filter.getPredicate()->toString().find("age") == std::string::npos) {
            continue;
        }
        if (op->getNumChildren() == 0) {
            continue;
        }
        auto* child = op->getChild(0).get();
        if (child->getOperatorType() == planner::LogicalOperatorType::SCAN_NODE_TABLE) {
            pushed = true;
        }
    }
    EXPECT_TRUE(pushed);
}

// The e2e runner and the default test DBs run at 2 threads, where the pre-fix plans
// returned correct results by luck: on the unfixed head the DISTINCT+LIMIT query
// returned 12 instead of 14 only at 8 and 16 threads. Re-run the repros repeatedly
// at 1, 8 and 16 threads on a dedicated 16-thread DB so CI catches the old bug.
class BoundExpandMultithreadTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto config = std::make_unique<main::SystemConfig>();
        config->maxNumThreads = 16;
        dbPath = TestHelper::getTempDBPathStr("BoundExpandMT");
        database = std::make_unique<main::Database>(dbPath, *config);
        conn = std::make_unique<main::Connection>(database.get());
        const auto datasetDir = TestHelper::appendLbugRootPath("dataset/tinysnb/");
        TestHelper::executeScript(datasetDir + TestHelper::SCHEMA_FILE_NAME, *conn);
        TestHelper::executeScript(datasetDir + TestHelper::COPY_FILE_NAME, *conn);
    }

    void TearDown() override {
        conn.reset();
        database.reset();
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(dbPath).parent_path(), ec);
    }

    std::string dbPath;
    std::unique_ptr<main::Database> database;
    std::unique_ptr<main::Connection> conn;
};

TEST_F(BoundExpandMultithreadTest, FactorizedOuterReadsBackCorrectCounts) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"MATCH (a:person)-[:knows]->(x:person) WITH DISTINCT a LIMIT 1000 "
         "MATCH (a)-[:knows]->(b:person) RETURN count(*)",
            "14"},
        {"MATCH (a:person)-[:knows]->(x:person) WITH a, count(*) AS c "
         "MATCH (a)-[:knows]->(b:person) WITH a, c, count(*) AS n WHERE c <> n "
         "RETURN count(*)",
            "0"},
    };
    for (const uint64_t threads : {1u, 8u, 16u}) {
        conn->setMaxNumThreadForExec(threads);
        for (auto run = 0; run < 10; ++run) {
            for (const auto& [query, expected] : cases) {
                auto result = conn->query(query);
                ASSERT_TRUE(result->isSuccess())
                    << threads << " threads, run " << run << ": " << result->getErrorMessage();
                EXPECT_EQ(std::vector<std::string>{expected},
                    TestHelper::convertResultToString(*result))
                    << threads << " threads, run " << run;
            }
        }
    }
}

} // namespace testing
} // namespace lbug

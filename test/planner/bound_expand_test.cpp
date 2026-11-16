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
    // No literal LIMIT and no ANALYZE stats on tinysnb: the credibility check fails
    // closed, so the GROUP BY outer stays on the hash-join path.
    auto plan = getRoot("MATCH (a:person)-[:knows]->(x:person) WITH a, count(*) AS c "
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

} // namespace testing
} // namespace lbug

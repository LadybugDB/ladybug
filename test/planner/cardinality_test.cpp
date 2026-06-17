#include "common/data_chunk/data_chunk_state.h"
#include "graph_test/private_graph_test.h"
#include "planner/operator/logical_plan_util.h"
#include "test_runner/test_runner.h"
#include <format>

namespace lbug {
namespace testing {

class CardinalityTest : public DBTest {
public:
    std::string getInputDir() override {
        return TestHelper::appendLbugRootPath("dataset/tinysnb/");
    }

    std::string getEncodedPlan(const std::string& query) {
        return planner::LogicalPlanUtil::encodeJoin(*TestRunner::getLogicalPlan(query, *conn));
    }
    std::unique_ptr<planner::LogicalPlan> getRoot(const std::string& query) {
        return TestRunner::getLogicalPlan(query, *conn);
    }
    std::pair<planner::LogicalOperator*, planner::LogicalOperator*> getSource(
        planner::LogicalOperator* op, planner::LogicalOperator* parent = nullptr) {
        if (op->getNumChildren() == 0) {
            return {parent, op};
        }
        return getSource(op->getChild(0).get(), op);
    }
    planner::LogicalOperator* getOpWithType(planner::LogicalOperator* op,
        planner::LogicalOperatorType type) {
        if (op->getOperatorType() == type) {
            return op;
        }
        if (op->getNumChildren() == 0) {
            return nullptr;
        }
        return getOpWithType(op->getChild(0).get(), type);
    }
    uint64_t countOpsWithType(planner::LogicalOperator* op, planner::LogicalOperatorType type) {
        auto result = op->getOperatorType() == type ? 1u : 0u;
        for (auto i = 0u; i < op->getNumChildren(); ++i) {
            result += countOpsWithType(op->getChild(i).get(), type);
        }
        return result;
    }
};

TEST_F(CardinalityTest, TestOperators) {
    // Filter
    {
        // we only get cardinalities of operators created by the optimizer if we use EXPLAIN LOGICAL
        auto plan = getRoot("EXPLAIN LOGICAL MATCH (p1: person) WHERE p1.gender=1 RETURN p1.ID");
        auto [parent, source] = getSource(plan->getLastOperator().get());
        EXPECT_EQ(planner::LogicalOperatorType::SCAN_NODE_TABLE, source->getOperatorType());
        EXPECT_EQ(8, source->getCardinality());
        EXPECT_EQ(planner::LogicalOperatorType::FILTER, parent->getOperatorType());
        EXPECT_EQ(4, parent->getCardinality());
    }

    // Limit
    {
        auto plan = getRoot("EXPLAIN LOGICAL MATCH (p1: person) RETURN p1.ID LIMIT 2");
        auto* limitOp =
            getOpWithType(plan->getLastOperator().get(), planner::LogicalOperatorType::LIMIT);
        ASSERT_NE(nullptr, limitOp);
        EXPECT_EQ(planner::LogicalOperatorType::LIMIT, limitOp->getOperatorType());
        EXPECT_EQ(2, limitOp->getCardinality());
    }

    // Aggregate
    {
        auto plan = getRoot("EXPLAIN LOGICAL MATCH (p1: person) RETURN COUNT(*), MAX(p1.age)");
        auto* aggregateOp =
            getOpWithType(plan->getLastOperator().get(), planner::LogicalOperatorType::AGGREGATE);
        ASSERT_NE(nullptr, aggregateOp);
        EXPECT_EQ(1, aggregateOp->getCardinality());
    }

    // Cross Product
    {
        auto plan = getRoot("EXPLAIN LOGICAL MATCH (p1: person), (p2: person) RETURN p1.ID, p2.ID");
        auto* productOp = getOpWithType(plan->getLastOperator().get(),
            planner::LogicalOperatorType::CROSS_PRODUCT);
        ASSERT_NE(nullptr, productOp);
        EXPECT_EQ(64, productOp->getCardinality());
    }

    // Hash Join (non-ID based join)
    {
        auto plan = getRoot("EXPLAIN LOGICAL MATCH (p: person), (o: organisation) WHERE p.ID = "
                            "o.ID RETURN p.fName, o.name");
        auto* joinOp =
            getOpWithType(plan->getLastOperator().get(), planner::LogicalOperatorType::HASH_JOIN);
        ASSERT_NE(nullptr, joinOp);
        EXPECT_EQ(1, joinOp->getCardinality());
    }

    // Extend + Hash Join
    {
        auto plan = getRoot(
            "EXPLAIN LOGICAL MATCH (p1: person)-[k:knows]->(p2: person) RETURN p1.ID, p2.ID");
        auto* extendOp =
            getOpWithType(plan->getLastOperator().get(), planner::LogicalOperatorType::EXTEND);
        ASSERT_NE(nullptr, extendOp);
        static constexpr auto numRelsInKnows = 14;
        EXPECT_EQ(numRelsInKnows, extendOp->getCardinality());

        auto* joinOp =
            getOpWithType(plan->getLastOperator().get(), planner::LogicalOperatorType::HASH_JOIN);
        ASSERT_NE(nullptr, joinOp);
        EXPECT_GT(joinOp->getCardinality(), 1);
    }

    // Intersect + Flatten
    {
        auto plan = getRoot(
            "EXPLAIN LOGICAL MATCH (p1: person)-[k1:knows]->(p2: person)-[k2:knows]->(p3:person), "
            "(p1)-[k3:knows]->(p3) "
            "HINT ((p1 JOIN k1 JOIN p2) MULTI_JOIN k2 MULTI_JOIN k3) JOIN p3 "
            "RETURN p1.ID, p2.ID, p3.ID");
        auto* intersect =
            getOpWithType(plan->getLastOperator().get(), planner::LogicalOperatorType::INTERSECT);
        ASSERT_NE(nullptr, intersect);
        EXPECT_EQ(intersect->getCardinality(), 1);

        auto* flatten =
            getOpWithType(plan->getLastOperator().get(), planner::LogicalOperatorType::FLATTEN);
        ASSERT_NE(nullptr, intersect);
        EXPECT_GT(flatten->getCardinality(), 1);
    }

    // Load From Parquet
    {
        auto plan = getRoot(std::format(
            "LOAD FROM \"{}/dataset/demo-db/parquet/user.parquet\" RETURN *", LBUG_ROOT_DIRECTORY));
        EXPECT_EQ(4, plan->getCardinality());
    }

    // Load From Numpy
    {
        auto plan = getRoot(std::format(
            "LOAD FROM \"{}/dataset/npy-1d/one_dim_int64.npy\" RETURN *", LBUG_ROOT_DIRECTORY));
        EXPECT_EQ(3, plan->getCardinality());
    }
}

TEST_F(CardinalityTest, TestPopulatedAfterOptimizations) {
    // Filter push down
    auto plan = getRoot("EXPLAIN LOGICAL MATCH (a:person)-[e]->(b) "
                        "WHERE a.ID < 0 AND a.fName='Alice' "
                        "RETURN a.gender;");
    std::function<void(planner::LogicalOperator*)> checkFunc;
    checkFunc = [&checkFunc](planner::LogicalOperator* op) {
        EXPECT_GT(op->getCardinality(), 0);
        for (uint32_t i = 0; i < op->getNumChildren(); ++i) {
            checkFunc(op->getChild(i).get());
        }
    };
    checkFunc(plan->getLastOperator().get());
}

TEST_F(CardinalityTest, TestPackedPathExtendOptIn) {
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE PackedPerson(id INT64, PRIMARY KEY(id));")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE REL TABLE PackedFollows(FROM PackedPerson TO PackedPerson);")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 1});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 2});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 3});")->isSuccess());
    const auto query = "EXPLAIN LOGICAL MATCH "
                       "(a:PackedPerson)-[:PackedFollows]->(b:PackedPerson)"
                       "-[:PackedFollows]->(c:PackedPerson) "
                       "RETURN a.id, b.id, c.id";

    ASSERT_TRUE(conn->query("CALL enable_packed_path_extend=false")->isSuccess());
    auto disabledPlan = getRoot(query);
    EXPECT_EQ(0, countOpsWithType(disabledPlan->getLastOperator().get(),
                     planner::LogicalOperatorType::PACKED_EXTEND));

    ASSERT_TRUE(conn->query("CALL enable_packed_path_extend=true")->isSuccess());
    auto enabledPlan = getRoot(query);
    EXPECT_GE(countOpsWithType(enabledPlan->getLastOperator().get(),
                  planner::LogicalOperatorType::PACKED_EXTEND),
        2);
}

TEST_F(CardinalityTest, TestPackedExtendDropsParentsWithoutMatches) {
    // Setup small packed graph where only one `a` has a valid two-hop path a->b->c.
    ASSERT_TRUE(conn->query("CREATE NODE TABLE PackedPerson(id INT64, PRIMARY KEY(id));")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE REL TABLE PackedFollows(FROM PackedPerson TO PackedPerson);")
                    ->isSuccess());
    // Create 5 nodes
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 1});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 2});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 3});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 4});")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:PackedPerson {id: 5});")->isSuccess());
    // Create edges forming a chain 1->2->3 only. Nodes 4 and 5 (and possibly 2) won't form full a->b->c
    ASSERT_TRUE(conn->query("MATCH (a:PackedPerson {id:1}), (b:PackedPerson {id:2}) CREATE (a)-[:PackedFollows]->(b);")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("MATCH (a:PackedPerson {id:2}), (b:PackedPerson {id:3}) CREATE (a)-[:PackedFollows]->(b);")
                    ->isSuccess());

    // Enable packed path extend and run the query. Expect only a.id == 1 to appear in results.
    ASSERT_TRUE(conn->query("CALL enable_packed_path_extend=true")->isSuccess());
    auto res = conn->query(
        "MATCH (a:PackedPerson)-[:PackedFollows]->(b:PackedPerson)-[:PackedFollows]->(c:PackedPerson) "
        "RETURN DISTINCT a.id ORDER BY a.id");
    auto tuple = res->getNext();
    ASSERT_NE(nullptr, tuple);
    EXPECT_EQ(1, tuple->getValue(0)->getValue<int64_t>());
    // No more distinct a ids
    EXPECT_EQ(nullptr, res->getNext());
}

TEST_F(CardinalityTest, TestPackedChildSliceState) {
    common::DataChunkState state;
    EXPECT_FALSE(state.hasPackedChildSlices());

    state.setSingleParentPackedChildSlice(3, 7);
    ASSERT_TRUE(state.hasPackedChildSlices());
    const auto& singleParentSlices = state.getPackedChildSlices();
    ASSERT_EQ(1, singleParentSlices.getNumParents());
    EXPECT_EQ(3, singleParentSlices.parentPositions[0]);
    EXPECT_EQ(0, singleParentSlices.offsets[0]);
    EXPECT_EQ(7, singleParentSlices.offsets[1]);
    EXPECT_EQ(7, singleParentSlices.getNumValues());

    state.setPackedChildSlices({1, 4}, {0, 2, 5});
    const auto& multiParentSlices = state.getPackedChildSlices();
    ASSERT_EQ(2, multiParentSlices.getNumParents());
    EXPECT_EQ(1, multiParentSlices.parentPositions[0]);
    EXPECT_EQ(4, multiParentSlices.parentPositions[1]);
    EXPECT_EQ(5, multiParentSlices.getNumValues());

    state.clearPackedChildSlices();
    EXPECT_FALSE(state.hasPackedChildSlices());
}

} // namespace testing
} // namespace lbug

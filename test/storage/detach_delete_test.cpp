#include "common/types/value/value.h"
#include "graph_test/private_graph_test.h"

namespace lbug {
namespace testing {

// Reproduces the "bad" case from issue 180: open a fresh Database+Connection on every
// iteration of the completion loop.  After all 1,200 iterations every edge and every node
// must have been deleted.
class DetachDeleteTest : public EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();

        // Create schema and seed data
        auto res = conn->query(R"(
            CREATE NODE TABLE D(id INT64 PRIMARY KEY);
            CREATE NODE TABLE E(id STRING DEFAULT gen_random_uuid(), PRIMARY KEY (id));
            CREATE NODE TABLE R(id INT64 PRIMARY KEY, c BOOLEAN DEFAULT false);
            CREATE REL TABLE t(FROM R TO D);
            CREATE REL TABLE b(FROM E TO R);
            UNWIND range(1, 1200) AS r
            CREATE (:E)-[:b]->(:R { id: r })-[:t]->(:D { id: r });
        )");
        ASSERT_TRUE(res->isSuccess()) << res->toString();
    }

    // Build a LIST(INT64) Value containing a single element
    static std::unique_ptr<common::Value> makeInt64ListValue(int64_t elem) {
        std::vector<std::unique_ptr<common::Value>> children;
        children.push_back(std::make_unique<common::Value>(elem));
        return std::make_unique<common::Value>(
            common::LogicalType::LIST(common::LogicalType::INT64()), std::move(children));
    }
};

TEST_F(DetachDeleteTest, BadCaseAllNodesAndEdgesDeleted) {
    if (inMemMode) {
        GTEST_SKIP();
    }

    // query1: mark R.c = true for the given id, then detach-delete any E nodes
    // whose only R children are now completed.
    const std::string query1Str = R"(
        UNWIND $ids AS id
        MATCH (e:E)-[:b]->(r:R)-[:t]->(:D { id: id })
        SET r.c = true
        WITH collect(e.id) AS ids
        MATCH (e:E)
        WHERE e.id IN ids AND NOT EXISTS {
            MATCH (e)-[:b]->(:R { c: false })
        }
        DETACH DELETE e
    )";

    // query2: iteratively delete R and D nodes that have no incoming edges.
    const std::string query2Str = R"(
        MATCH (n:R:D)
        WHERE NOT EXISTS { MATCH ()-[]->(n) }
        DETACH DELETE n
        RETURN true
    )";

    // "bad" mode: fresh Database + Connection on every iteration
    for (int i = 1; i <= 1200; i++) {
        // Drop the current connection/database so the next open starts fresh.
        conn.reset();
        database.reset();

        createDBAndConn();

        auto stmt1 = conn->prepare(query1Str);
        ASSERT_TRUE(stmt1->isSuccess()) << stmt1->getErrorMessage();

        std::unordered_map<std::string, std::unique_ptr<common::Value>> params1;
        params1["ids"] = makeInt64ListValue(static_cast<int64_t>(i));
        auto r1 = conn->executeWithParams(stmt1.get(), std::move(params1));
        ASSERT_TRUE(r1->isSuccess()) << r1->toString();

        auto stmt2 = conn->prepare(query2Str);
        ASSERT_TRUE(stmt2->isSuccess()) << stmt2->getErrorMessage();

        while (true) {
            auto r2 = conn->execute(stmt2.get());
            ASSERT_TRUE(r2->isSuccess()) << r2->toString();
            if (r2->getNumTuples() == 0) {
                break;
            }
        }
    }
    createDBAndConn();

    // Final verification: all edges and all nodes must be gone.
    auto countEdgesT = conn->query("MATCH ()-[:t]->() RETURN count(*)");
    ASSERT_TRUE(countEdgesT->isSuccess());
    EXPECT_EQ(0, countEdgesT->getNext()->getValue(0)->getValue<int64_t>());

    auto countEdgesB = conn->query("MATCH ()-[:b]->() RETURN count(*)");
    ASSERT_TRUE(countEdgesB->isSuccess());
    EXPECT_EQ(0, countEdgesB->getNext()->getValue(0)->getValue<int64_t>());

    auto countD = conn->query("MATCH (n:D) RETURN count(*)");
    ASSERT_TRUE(countD->isSuccess());
    EXPECT_EQ(0, countD->getNext()->getValue(0)->getValue<int64_t>());

    auto countE = conn->query("MATCH (n:E) RETURN count(*)");
    ASSERT_TRUE(countE->isSuccess());
    EXPECT_EQ(0, countE->getNext()->getValue(0)->getValue<int64_t>());

    auto countR = conn->query("MATCH (n:R) RETURN count(*)");
    ASSERT_TRUE(countR->isSuccess());
    EXPECT_EQ(0, countR->getNext()->getValue(0)->getValue<int64_t>());
}

} // namespace testing
} // namespace lbug

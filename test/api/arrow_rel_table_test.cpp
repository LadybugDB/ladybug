#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "arrow_test_utils.h"
#include "common/arrow/arrow.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "storage/table/arrow_table_support.h"

using namespace lbug;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static void createArrowPersonTable(main::Connection& connection) {
    std::vector<int64_t> ids = {1, 2, 3};
    std::vector<std::string> names = {"Alice", "Bob", "Carol"};

    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 2);
    createSchema<int64_t>(schema.children[0], "id");
    createSchema<std::string>(schema.children[1], "name");

    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(createStructArray(ids.size(),
        {[&](ArrowArray* array) { createInt64Array(array, ids); },
            [&](ArrowArray* array) { createStringArray(array, names); }}));

    auto result = ArrowTableSupport::createViewFromArrowTable(connection, "arrow_rel_person",
        std::move(schema), std::move(arrays));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
}

static void createNativePersonTable(main::Connection& connection) {
    auto result = connection.query(
        "CREATE NODE TABLE arrow_rel_person(id INT64, name STRING, PRIMARY KEY(id));"
        "CREATE (:arrow_rel_person {id: 1, name: 'Alice'});"
        "CREATE (:arrow_rel_person {id: 2, name: 'Bob'});"
        "CREATE (:arrow_rel_person {id: 3, name: 'Carol'});");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
}

static void createArrowKnowsTable(main::Connection& connection) {
    std::vector<int64_t> from = {1, 1, 2};
    std::vector<int64_t> to = {2, 3, 3};
    std::vector<int64_t> weight = {10, 20, 30};

    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 3);
    createSchema<int64_t>(schema.children[0], "from");
    createSchema<int64_t>(schema.children[1], "to");
    createSchema<int64_t>(schema.children[2], "weight");

    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(createStructArray(from.size(),
        {[&](ArrowArray* array) { createInt64Array(array, from); },
            [&](ArrowArray* array) { createInt64Array(array, to); },
            [&](ArrowArray* array) { createInt64Array(array, weight); }}));

    auto result = ArrowTableSupport::createRelTableFromArrowTable(connection, "arrow_rel_knows",
        "arrow_rel_person", "arrow_rel_person", std::move(schema), std::move(arrays));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
}

// ─────────────────────────────────────────────────────────────────────────────
// Basic edge-list scan tests
// ─────────────────────────────────────────────────────────────────────────────

class ArrowRelTableTest : public lbug::testing::EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }
};

TEST_F(ArrowRelTableTest, ScanArrowRelTableOverArrowNodeTable) {
    createArrowPersonTable(*conn);
    createArrowKnowsTable(*conn);

    auto countResult = conn->query(
        "MATCH (:arrow_rel_person)-[:arrow_rel_knows]->(:arrow_rel_person) RETURN count(*)");
    ASSERT_TRUE(countResult->isSuccess()) << countResult->getErrorMessage();
    ASSERT_EQ(countResult->getNext()->getValue(0)->getValue<int64_t>(), 3);

    auto sumResult = conn->query(
        "MATCH (:arrow_rel_person)-[e:arrow_rel_knows]->(:arrow_rel_person) RETURN sum(e.weight)");
    ASSERT_TRUE(sumResult->isSuccess()) << sumResult->getErrorMessage();
    ASSERT_EQ(sumResult->getNext()->getValue(0)->getValue<common::int128_t>(), 60);
}

TEST_F(ArrowRelTableTest, ScanArrowRelTableOverNativeNodeTable) {
    createNativePersonTable(*conn);
    createArrowKnowsTable(*conn);

    auto countResult = conn->query(
        "MATCH (:arrow_rel_person)-[:arrow_rel_knows]->(:arrow_rel_person) RETURN count(*)");
    ASSERT_TRUE(countResult->isSuccess()) << countResult->getErrorMessage();
    ASSERT_EQ(countResult->getNext()->getValue(0)->getValue<int64_t>(), 3);

    auto sumResult = conn->query(
        "MATCH (:arrow_rel_person)-[e:arrow_rel_knows]->(:arrow_rel_person) RETURN sum(e.weight)");
    ASSERT_TRUE(sumResult->isSuccess()) << sumResult->getErrorMessage();
    ASSERT_EQ(sumResult->getNext()->getValue(0)->getValue<common::int128_t>(), 60);
}

TEST_F(ArrowRelTableTest, ScanMixedArrowAndNativeRelTables) {
    createArrowPersonTable(*conn);
    createArrowKnowsTable(*conn);

    auto createNativeTables =
        conn->query("CREATE NODE TABLE arrow_node_account(id INT64, PRIMARY KEY(id));"
                    "CREATE REL TABLE arrow_rel_transfer(FROM arrow_node_account TO "
                    "arrow_node_account);"
                    "CREATE (:arrow_node_account {id: 10})-[:arrow_rel_transfer]->"
                    "(:arrow_node_account {id: 20});");
    ASSERT_TRUE(createNativeTables->isSuccess()) << createNativeTables->getErrorMessage();

    auto result = conn->query("MATCH ()-[]->() RETURN count(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 4);
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-batch edge-list tests
// ─────────────────────────────────────────────────────────────────────────────

// 3 person nodes (1 batch), knows rel table with 2 Arrow batches:
//   batch0: [1→2 w=10, 1→3 w=20], batch1: [2→3 w=30]  count=3, sum=60

TEST_F(ArrowRelTableTest, MultiBatchArrowRelTable) {
    createArrowPersonTable(*conn);

    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 3);
    createSchema<int64_t>(schema.children[0], "from");
    createSchema<int64_t>(schema.children[1], "to");
    createSchema<int64_t>(schema.children[2], "weight");

    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(
        createStructArray(2, {[](ArrowArray* a) { createInt64Array(a, {1, 1}); },
                                 [](ArrowArray* a) { createInt64Array(a, {2, 3}); },
                                 [](ArrowArray* a) { createInt64Array(a, {10, 20}); }}));
    arrays.push_back(createStructArray(1, {[](ArrowArray* a) { createInt64Array(a, {2}); },
                                              [](ArrowArray* a) { createInt64Array(a, {3}); },
                                              [](ArrowArray* a) { createInt64Array(a, {30}); }}));

    auto result = ArrowTableSupport::createRelTableFromArrowTable(*conn, "arrow_rel_knows",
        "arrow_rel_person", "arrow_rel_person", std::move(schema), std::move(arrays));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();

    auto countResult = conn->query(
        "MATCH (:arrow_rel_person)-[:arrow_rel_knows]->(:arrow_rel_person) RETURN count(*)");
    ASSERT_TRUE(countResult->isSuccess()) << countResult->getErrorMessage();
    ASSERT_EQ(countResult->getNext()->getValue(0)->getValue<int64_t>(), 3);

    auto sumResult = conn->query(
        "MATCH (:arrow_rel_person)-[e:arrow_rel_knows]->(:arrow_rel_person) RETURN sum(e.weight)");
    ASSERT_TRUE(sumResult->isSuccess()) << sumResult->getErrorMessage();
    ASSERT_EQ(sumResult->getNext()->getValue(0)->getValue<common::int128_t>(), 60);
}

TEST_F(ArrowRelTableTest, MultiBatchArrowRelTableBwdScan) {
    createNativePersonTable(*conn);

    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 3);
    createSchema<int64_t>(schema.children[0], "from");
    createSchema<int64_t>(schema.children[1], "to");
    createSchema<int64_t>(schema.children[2], "weight");

    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(
        createStructArray(2, {[](ArrowArray* a) { createInt64Array(a, {1, 1}); },
                                 [](ArrowArray* a) { createInt64Array(a, {2, 3}); },
                                 [](ArrowArray* a) { createInt64Array(a, {10, 20}); }}));
    arrays.push_back(createStructArray(1, {[](ArrowArray* a) { createInt64Array(a, {2}); },
                                              [](ArrowArray* a) { createInt64Array(a, {3}); },
                                              [](ArrowArray* a) { createInt64Array(a, {30}); }}));

    auto result = ArrowTableSupport::createRelTableFromArrowTable(*conn, "arrow_rel_knows",
        "arrow_rel_person", "arrow_rel_person", std::move(schema), std::move(arrays));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();

    auto countResult = conn->query(
        "MATCH (:arrow_rel_person)<-[:arrow_rel_knows]-(:arrow_rel_person) RETURN count(*)");
    ASSERT_TRUE(countResult->isSuccess()) << countResult->getErrorMessage();
    ASSERT_EQ(countResult->getNext()->getValue(0)->getValue<int64_t>(), 3);
}

// Large-batch: 2050 nodes, 2049 chain edges split into 2 batches (2048 + 1).
// batch0 has more rows than DEFAULT_VECTOR_CAPACITY (2048), forcing ScanRelTable
// to do two rounds and testing Arrow batch advancement mid-scan.
// sum(0..2048) = 2048*2049/2 = 2098176
TEST_F(ArrowRelTableTest, LargeBatchArrowRelTable) {
    constexpr int64_t NUM_NODES = 2050;
    constexpr int64_t NUM_EDGES = 2049;
    constexpr int64_t SPLIT = 2048; // batch0 row count

    {
        ArrowSchemaWrapper schema;
        createStructSchema(&schema, 1);
        createSchema<int64_t>(schema.children[0], "id");
        std::vector<int64_t> ids(NUM_NODES);
        std::iota(ids.begin(), ids.end(), int64_t(0));
        std::vector<ArrowArrayWrapper> batches;
        batches.push_back(
            createStructArray(NUM_NODES, {[&](ArrowArray* a) { createInt64Array(a, ids); }}));
        auto r = ArrowTableSupport::createViewFromArrowTable(*conn, "lb_person", std::move(schema),
            std::move(batches));
        ASSERT_TRUE(r.queryResult->isSuccess()) << r.queryResult->getErrorMessage();
    }

    {
        ArrowSchemaWrapper schema;
        createStructSchema(&schema, 3);
        createSchema<int64_t>(schema.children[0], "from");
        createSchema<int64_t>(schema.children[1], "to");
        createSchema<int64_t>(schema.children[2], "weight");

        std::vector<int64_t> frm0(SPLIT), to0(SPLIT), w0(SPLIT);
        for (int64_t i = 0; i < SPLIT; ++i) {
            frm0[i] = i;
            to0[i] = i + 1;
            w0[i] = i;
        }

        std::vector<ArrowArrayWrapper> batches;
        batches.push_back(
            createStructArray(SPLIT, {[&](ArrowArray* a) { createInt64Array(a, frm0); },
                                         [&](ArrowArray* a) { createInt64Array(a, to0); },
                                         [&](ArrowArray* a) { createInt64Array(a, w0); }}));
        // batch1: single trailing edge
        batches.push_back(
            createStructArray(1, {[](ArrowArray* a) { createInt64Array(a, {2048}); },
                                     [](ArrowArray* a) { createInt64Array(a, {2049}); },
                                     [](ArrowArray* a) { createInt64Array(a, {2048}); }}));

        auto r = ArrowTableSupport::createRelTableFromArrowTable(*conn, "lb_chain", "lb_person",
            "lb_person", std::move(schema), std::move(batches));
        ASSERT_TRUE(r.queryResult->isSuccess()) << r.queryResult->getErrorMessage();
    }

    auto countResult = conn->query("MATCH (:lb_person)-[:lb_chain]->(:lb_person) RETURN count(*)");
    ASSERT_TRUE(countResult->isSuccess()) << countResult->getErrorMessage();
    ASSERT_EQ(countResult->getNext()->getValue(0)->getValue<int64_t>(), NUM_EDGES);

    // sum(0..2048) = 2098176
    auto sumResult =
        conn->query("MATCH (:lb_person)-[e:lb_chain]->(:lb_person) RETURN sum(e.weight)");
    ASSERT_TRUE(sumResult->isSuccess()) << sumResult->getErrorMessage();
    ASSERT_EQ(sumResult->getNext()->getValue(0)->getValue<common::int128_t>(), 2098176);
}

// ─────────────────────────────────────────────────────────────────────────────
// Complex graph tests
// ─────────────────────────────────────────────────────────────────────────────

// Graph:
//   user: Noura(75)→offset0, Adam(100)→offset1, Karissa(250)→offset2, Zhang(300)→offset3
//   city: Guelph(500)→offset0, Kitchener(600)→offset1, Waterloo(700)→offset2
//   follows(user→user): 7 edges including self-loop Adam→Adam
//   livesin(user→city): 4 edges; each user has exactly one city

class ArrowRelTableComplexTest : public lbug::testing::EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
        createAllTables();
    }

    void createAllTables() {
        createUserTable();
        createCityTable();
        createFollowsTable();
        createLivesInTable();
    }

    void createUserTable() {
        std::vector<int64_t> ids = {75, 100, 250, 300};
        ArrowSchemaWrapper schema;
        createStructSchema(&schema, 1);
        createSchema<int64_t>(schema.children[0], "id");
        std::vector<ArrowArrayWrapper> arrays;
        arrays.push_back(createStructArray(4, {[&](ArrowArray* a) { createInt64Array(a, ids); }}));
        auto r = ArrowTableSupport::createViewFromArrowTable(*conn, "cx_user", std::move(schema),
            std::move(arrays));
        ASSERT_TRUE(r.queryResult->isSuccess()) << r.queryResult->getErrorMessage();
    }

    void createCityTable() {
        std::vector<int64_t> ids = {500, 600, 700};
        ArrowSchemaWrapper schema;
        createStructSchema(&schema, 1);
        createSchema<int64_t>(schema.children[0], "id");
        std::vector<ArrowArrayWrapper> arrays;
        arrays.push_back(createStructArray(3, {[&](ArrowArray* a) { createInt64Array(a, ids); }}));
        auto r = ArrowTableSupport::createViewFromArrowTable(*conn, "cx_city", std::move(schema),
            std::move(arrays));
        ASSERT_TRUE(r.queryResult->isSuccess()) << r.queryResult->getErrorMessage();
    }

    void createFollowsTable() {
        // 7 edges; self-loop Adam(100)→Adam(100)
        std::vector<int64_t> from = {75, 100, 100, 100, 250, 250, 300};
        std::vector<int64_t> to = {100, 100, 250, 300, 100, 300, 75};
        std::vector<int64_t> year = {2023, 2023, 2020, 2020, 2022, 2021, 2022};
        ArrowSchemaWrapper schema;
        createStructSchema(&schema, 3);
        createSchema<int64_t>(schema.children[0], "from");
        createSchema<int64_t>(schema.children[1], "to");
        createSchema<int64_t>(schema.children[2], "year");
        std::vector<ArrowArrayWrapper> arrays;
        arrays.push_back(
            createStructArray(7, {[&](ArrowArray* a) { createInt64Array(a, from); },
                                     [&](ArrowArray* a) { createInt64Array(a, to); },
                                     [&](ArrowArray* a) { createInt64Array(a, year); }}));
        auto r = ArrowTableSupport::createRelTableFromArrowTable(*conn, "cx_follows", "cx_user",
            "cx_user", std::move(schema), std::move(arrays));
        ASSERT_TRUE(r.queryResult->isSuccess()) << r.queryResult->getErrorMessage();
    }

    void createLivesInTable() {
        // Noura→Guelph(500), Adam→Waterloo(700), Karissa→Waterloo(700), Zhang→Kitchener(600)
        std::vector<int64_t> from = {75, 100, 250, 300};
        std::vector<int64_t> to = {500, 700, 700, 600};
        ArrowSchemaWrapper schema;
        createStructSchema(&schema, 2);
        createSchema<int64_t>(schema.children[0], "from");
        createSchema<int64_t>(schema.children[1], "to");
        std::vector<ArrowArrayWrapper> arrays;
        arrays.push_back(
            createStructArray(4, {[&](ArrowArray* a) { createInt64Array(a, from); },
                                     [&](ArrowArray* a) { createInt64Array(a, to); }}));
        auto r = ArrowTableSupport::createRelTableFromArrowTable(*conn, "cx_livesin", "cx_user",
            "cx_city", std::move(schema), std::move(arrays));
        ASSERT_TRUE(r.queryResult->isSuccess()) << r.queryResult->getErrorMessage();
    }
};

TEST_F(ArrowRelTableComplexTest, FwdFollowsCount) {
    auto result = conn->query("MATCH (:cx_user)-[:cx_follows]->(:cx_user) RETURN count(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 7);
}

TEST_F(ArrowRelTableComplexTest, BwdFollowsCount) {
    auto result = conn->query("MATCH (:cx_user)<-[:cx_follows]-(:cx_user) RETURN count(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 7);
}

TEST_F(ArrowRelTableComplexTest, UndirectedLivesInCount) {
    auto result = conn->query("MATCH (:cx_user)-[:cx_livesin]->(:cx_city) RETURN count(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 4);
}

TEST_F(ArrowRelTableComplexTest, SelfLoopFollowsCount) {
    auto result = conn->query("MATCH (n:cx_user)-[:cx_follows]->(n) RETURN count(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
}

TEST_F(ArrowRelTableComplexTest, TwoHopFollowsThenLivesIn) {
    // For each follows edge A→B, B must have a livesin edge. All 4 users have livesin → 7 results.
    auto result = conn->query(
        "MATCH (:cx_user)-[:cx_follows]->(:cx_user)-[:cx_livesin]->(:cx_city) RETURN count(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 7);
}

TEST_F(ArrowRelTableComplexTest, BwdFollowsThenFwdLivesIn) {
    // (a:user)<-[:follows]-(b:user)-[:livesin]->(c:city): 7 follows × 1 livesin per src = 7
    auto result = conn->query(
        "MATCH (:cx_user)<-[:cx_follows]-(:cx_user)-[:cx_livesin]->(:cx_city) RETURN count(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 7);
}

TEST_F(ArrowRelTableComplexTest, FollowsYearSumFwdAndBwd) {
    // years: 2023+2023+2020+2020+2022+2021+2022 = 14151
    auto fwdSum = conn->query("MATCH (:cx_user)-[e:cx_follows]->(:cx_user) RETURN sum(e.year)");
    ASSERT_TRUE(fwdSum->isSuccess()) << fwdSum->getErrorMessage();
    ASSERT_EQ(fwdSum->getNext()->getValue(0)->getValue<common::int128_t>(), 14151);

    auto bwdSum = conn->query("MATCH (:cx_user)<-[e:cx_follows]-(:cx_user) RETURN sum(e.year)");
    ASSERT_TRUE(bwdSum->isSuccess()) << bwdSum->getErrorMessage();
    ASSERT_EQ(bwdSum->getNext()->getValue(0)->getValue<common::int128_t>(), 14151);
}

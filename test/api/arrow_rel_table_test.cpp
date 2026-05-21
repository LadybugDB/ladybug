#include <memory>
#include <numeric>
#include <string>
#include <tuple>
#include <vector>

#include "arrow_test_utils.h"
#include "common/arrow/arrow.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "storage/table/arrow_table_support.h"

using namespace lbug;

namespace {

constexpr int32_t DATE_2019_01_01 = 17897;
constexpr int32_t DATE_2020_01_01 = 18262;
constexpr int32_t DATE_2021_01_01 = 18628;
constexpr int32_t DATE_2022_01_01 = 18993;
constexpr int32_t DATE_2023_01_01 = 19358;
constexpr int32_t DATE_2024_01_01 = 19723;

struct PersonRow {
    int64_t id;
    const char* name;
    int64_t age;
    int32_t joinDate;
    std::vector<int64_t> scores;
};

struct CityRow {
    int64_t id;
    const char* name;
    int64_t population;
    int32_t founded;
    std::vector<int64_t> tags;
};

struct KnowsRow {
    int64_t from;
    int64_t to;
    int64_t weight;
    const char* label;
    int32_t since;
    std::vector<int64_t> hops;
};

struct LivesInRow {
    int64_t from;
    int64_t to;
    int32_t since;
    std::vector<int64_t> importance;
};

const std::vector<PersonRow>& getComplexPersonBatch0() {
    static const std::vector<PersonRow> rows = {{1, "Alice", 25, DATE_2020_01_01, {100, 200}},
        {2, "Bob", 30, DATE_2021_01_01, {300}}, {3, "Carol", 40, DATE_2022_01_01, {100, 200, 300}}};
    return rows;
}

const std::vector<PersonRow>& getComplexPersonBatch1() {
    static const std::vector<PersonRow> rows = {{4, "Dave", 50, DATE_2023_01_01, {400, 500}},
        {5, "Eve", 35, DATE_2024_01_01, {100}}};
    return rows;
}

const std::vector<CityRow>& getCityBatch0() {
    static const std::vector<CityRow> rows = {{500, "Guelph", 75000, DATE_2022_01_01, {1}},
        {600, "Kitchener", 200000, DATE_2021_01_01, {2, 3}}};
    return rows;
}

const std::vector<CityRow>& getCityBatch1() {
    static const std::vector<CityRow> rows = {{700, "Waterloo", 150000, DATE_2020_01_01, {1, 2}}};
    return rows;
}

const std::vector<KnowsRow>& getKnowsBatch0() {
    static const std::vector<KnowsRow> rows = {{1, 2, 10, "friend", DATE_2020_01_01, {1}},
        {1, 3, 20, "colleague", DATE_2021_01_01, {1, 2}},
        {2, 3, 30, "friend", DATE_2022_01_01, {1}}};
    return rows;
}

const std::vector<KnowsRow>& getKnowsBatch1() {
    static const std::vector<KnowsRow> rows = {{2, 4, 40, "mentor", DATE_2019_01_01, {2, 3}},
        {3, 5, 15, "friend", DATE_2023_01_01, {1}}};
    return rows;
}

const std::vector<LivesInRow>& getLivesInBatch0() {
    static const std::vector<LivesInRow> rows = {{1, 700, DATE_2020_01_01, {1}},
        {2, 700, DATE_2021_01_01, {1, 2}}};
    return rows;
}

const std::vector<LivesInRow>& getLivesInBatch1() {
    static const std::vector<LivesInRow> rows = {{3, 600, DATE_2022_01_01, {2}},
        {4, 500, DATE_2019_01_01, {3}}};
    return rows;
}

ArrowSchemaWrapper makeComplexPersonSchema() {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 5);
    createSchema<int64_t>(schema.children[0], "id");
    createSchema<std::string>(schema.children[1], "name");
    createSchema<int64_t>(schema.children[2], "age");
    createDateSchema(schema.children[3], "join_date");
    createListInt64Schema(schema.children[4], "scores");
    return schema;
}

ArrowSchemaWrapper makeCitySchema() {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 5);
    createSchema<int64_t>(schema.children[0], "id");
    createSchema<std::string>(schema.children[1], "name");
    createSchema<int64_t>(schema.children[2], "population");
    createDateSchema(schema.children[3], "founded");
    createListInt64Schema(schema.children[4], "tags");
    return schema;
}

ArrowSchemaWrapper makeKnowsSchema() {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 6);
    createSchema<int64_t>(schema.children[0], "from");
    createSchema<int64_t>(schema.children[1], "to");
    createSchema<int64_t>(schema.children[2], "weight");
    createSchema<std::string>(schema.children[3], "label");
    createDateSchema(schema.children[4], "since");
    createListInt64Schema(schema.children[5], "hops");
    return schema;
}

ArrowSchemaWrapper makeLivesInSchema() {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 4);
    createSchema<int64_t>(schema.children[0], "from");
    createSchema<int64_t>(schema.children[1], "to");
    createDateSchema(schema.children[2], "since");
    createListInt64Schema(schema.children[3], "importance");
    return schema;
}

ArrowArrayWrapper makePersonBatch(const std::vector<PersonRow>& rows) {
    std::vector<int64_t> ids;
    std::vector<std::string> names;
    std::vector<int64_t> ages;
    std::vector<int32_t> joinDates;
    std::vector<std::vector<int64_t>> scores;
    for (const auto& row : rows) {
        ids.push_back(row.id);
        names.emplace_back(row.name);
        ages.push_back(row.age);
        joinDates.push_back(row.joinDate);
        scores.push_back(row.scores);
    }
    return createStructArray(static_cast<int64_t>(rows.size()),
        {[&](ArrowArray* array) { createInt64Array(array, ids); },
            [&](ArrowArray* array) { createStringArray(array, names); },
            [&](ArrowArray* array) { createInt64Array(array, ages); },
            [&](ArrowArray* array) { createDateArray(array, joinDates); },
            [&](ArrowArray* array) { createListInt64Array(array, scores); }});
}

ArrowArrayWrapper makeCityBatch(const std::vector<CityRow>& rows) {
    std::vector<int64_t> ids;
    std::vector<std::string> names;
    std::vector<int64_t> populations;
    std::vector<int32_t> founded;
    std::vector<std::vector<int64_t>> tags;
    for (const auto& row : rows) {
        ids.push_back(row.id);
        names.emplace_back(row.name);
        populations.push_back(row.population);
        founded.push_back(row.founded);
        tags.push_back(row.tags);
    }
    return createStructArray(static_cast<int64_t>(rows.size()),
        {[&](ArrowArray* array) { createInt64Array(array, ids); },
            [&](ArrowArray* array) { createStringArray(array, names); },
            [&](ArrowArray* array) { createInt64Array(array, populations); },
            [&](ArrowArray* array) { createDateArray(array, founded); },
            [&](ArrowArray* array) { createListInt64Array(array, tags); }});
}

ArrowArrayWrapper makeKnowsBatch(const std::vector<KnowsRow>& rows) {
    std::vector<int64_t> from;
    std::vector<int64_t> to;
    std::vector<int64_t> weights;
    std::vector<std::string> labels;
    std::vector<int32_t> since;
    std::vector<std::vector<int64_t>> hops;
    for (const auto& row : rows) {
        from.push_back(row.from);
        to.push_back(row.to);
        weights.push_back(row.weight);
        labels.emplace_back(row.label);
        since.push_back(row.since);
        hops.push_back(row.hops);
    }
    return createStructArray(static_cast<int64_t>(rows.size()),
        {[&](ArrowArray* array) { createInt64Array(array, from); },
            [&](ArrowArray* array) { createInt64Array(array, to); },
            [&](ArrowArray* array) { createInt64Array(array, weights); },
            [&](ArrowArray* array) { createStringArray(array, labels); },
            [&](ArrowArray* array) { createDateArray(array, since); },
            [&](ArrowArray* array) { createListInt64Array(array, hops); }});
}

ArrowArrayWrapper makeLivesInBatch(const std::vector<LivesInRow>& rows) {
    std::vector<int64_t> from;
    std::vector<int64_t> to;
    std::vector<int32_t> since;
    std::vector<std::vector<int64_t>> importance;
    for (const auto& row : rows) {
        from.push_back(row.from);
        to.push_back(row.to);
        since.push_back(row.since);
        importance.push_back(row.importance);
    }
    return createStructArray(static_cast<int64_t>(rows.size()),
        {[&](ArrowArray* array) { createInt64Array(array, from); },
            [&](ArrowArray* array) { createInt64Array(array, to); },
            [&](ArrowArray* array) { createDateArray(array, since); },
            [&](ArrowArray* array) { createListInt64Array(array, importance); }});
}

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

void createComplexArrowPersonTable(main::Connection& connection,
    const std::string& tableName = "person") {
    auto schema = makeComplexPersonSchema();
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(makePersonBatch(getComplexPersonBatch0()));
    arrays.push_back(makePersonBatch(getComplexPersonBatch1()));
    auto result = ArrowTableSupport::createViewFromArrowTable(connection, tableName,
        std::move(schema), std::move(arrays));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
}

void createComplexArrowKnowsTable(main::Connection& connection,
    const std::string& tableName = "knows", const std::string& srcTableName = "person",
    const std::string& dstTableName = "person") {
    auto schema = makeKnowsSchema();
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(makeKnowsBatch(getKnowsBatch0()));
    arrays.push_back(makeKnowsBatch(getKnowsBatch1()));
    auto result = ArrowTableSupport::createRelTableFromArrowTable(connection, tableName,
        srcTableName, dstTableName, std::move(schema), std::move(arrays));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
}

void createComplexArrowCityTable(main::Connection& connection,
    const std::string& tableName = "city") {
    auto schema = makeCitySchema();
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(makeCityBatch(getCityBatch0()));
    arrays.push_back(makeCityBatch(getCityBatch1()));
    auto result = ArrowTableSupport::createViewFromArrowTable(connection, tableName,
        std::move(schema), std::move(arrays));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
}

void createComplexArrowLivesInTable(main::Connection& connection,
    const std::string& tableName = "livesin", const std::string& srcTableName = "person",
    const std::string& dstTableName = "city") {
    auto schema = makeLivesInSchema();
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(makeLivesInBatch(getLivesInBatch0()));
    arrays.push_back(makeLivesInBatch(getLivesInBatch1()));
    auto result = ArrowTableSupport::createRelTableFromArrowTable(connection, tableName,
        srcTableName, dstTableName, std::move(schema), std::move(arrays));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
}

void createComplexNativePersonTable(main::Connection& connection,
    const std::string& tableName = "person") {
    auto result =
        connection.query("CREATE NODE TABLE " + tableName +
                         "(id INT64, name STRING, age INT64, join_date DATE, "
                         "scores INT64[], PRIMARY KEY(id));"
                         "CREATE (:" +
                         tableName +
                         " {id: 1, name: 'Alice', age: 25, join_date: date('2020-01-01'), "
                         "scores: [100, 200]});"
                         "CREATE (:" +
                         tableName +
                         " {id: 2, name: 'Bob', age: 30, join_date: date('2021-01-01'), "
                         "scores: [300]});"
                         "CREATE (:" +
                         tableName +
                         " {id: 3, name: 'Carol', age: 40, join_date: date('2022-01-01'), "
                         "scores: [100, 200, 300]});"
                         "CREATE (:" +
                         tableName +
                         " {id: 4, name: 'Dave', age: 50, join_date: date('2023-01-01'), "
                         "scores: [400, 500]});"
                         "CREATE (:" +
                         tableName +
                         " {id: 5, name: 'Eve', age: 35, join_date: date('2024-01-01'), "
                         "scores: [100]});");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
}

void createLargeBatchComplexGraph(main::Connection& connection) {
    constexpr int64_t NUM_NODES = 2050;
    constexpr int64_t NUM_EDGES = 2049;
    constexpr int64_t NODE_SPLIT = 1025;
    constexpr int64_t EDGE_SPLIT = 1025;

    {
        auto schema = makeComplexPersonSchema();
        std::vector<PersonRow> batch0;
        std::vector<PersonRow> batch1;
        for (int64_t i = 0; i < NUM_NODES; ++i) {
            auto row = PersonRow{i, "Person", 20 + (i % 40),
                DATE_2020_01_01 + static_cast<int32_t>(i % 5), {i, i + 1}};
            if (i < NODE_SPLIT) {
                batch0.push_back(row);
            } else {
                batch1.push_back(row);
            }
        }
        std::vector<ArrowArrayWrapper> arrays;
        arrays.push_back(makePersonBatch(batch0));
        arrays.push_back(makePersonBatch(batch1));
        auto result = ArrowTableSupport::createViewFromArrowTable(connection, "lb_person",
            std::move(schema), std::move(arrays));
        ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
    }

    {
        auto schema = makeKnowsSchema();
        std::vector<KnowsRow> batch0;
        std::vector<KnowsRow> batch1;
        for (int64_t i = 0; i < NUM_EDGES; ++i) {
            KnowsRow row{i, i + 1, i, i < 3 ? "first3" : "rest",
                i < 5 ? DATE_2020_01_01 : DATE_2021_01_01, {i % 3}};
            if (i < EDGE_SPLIT) {
                batch0.push_back(row);
            } else {
                batch1.push_back(row);
            }
        }
        std::vector<ArrowArrayWrapper> arrays;
        arrays.push_back(makeKnowsBatch(batch0));
        arrays.push_back(makeKnowsBatch(batch1));
        auto result = ArrowTableSupport::createRelTableFromArrowTable(connection, "lb_chain",
            "lb_person", "lb_person", std::move(schema), std::move(arrays));
        ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
    }
}

} // namespace

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

TEST_F(ArrowRelTableTest, LargeBatchArrowRelTable) {
    constexpr int64_t NUM_NODES = 2050;
    constexpr int64_t NUM_EDGES = 2049;
    constexpr int64_t SPLIT = 2048;

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

    auto sumResult =
        conn->query("MATCH (:lb_person)-[e:lb_chain]->(:lb_person) RETURN sum(e.weight)");
    ASSERT_TRUE(sumResult->isSuccess()) << sumResult->getErrorMessage();
    ASSERT_EQ(sumResult->getNext()->getValue(0)->getValue<common::int128_t>(), 2098176);
}

class ArrowRelTableComplexTypesTest : public lbug::testing::EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }
};

TEST_F(ArrowRelTableComplexTypesTest, MultiBatchNodesAndRelsWithComplexTypes) {
    createComplexArrowPersonTable(*conn);
    createComplexArrowKnowsTable(*conn);

    auto result = conn->query("MATCH (a:person)-[e:knows]->(b:person) WHERE e.label = 'friend' "
                              "RETURN a.name, b.name, e.weight ORDER BY e.weight");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    const std::vector<std::tuple<std::string, std::string, int64_t>> expected = {
        {"Alice", "Bob", 10}, {"Carol", "Eve", 15}, {"Bob", "Carol", 30}};
    for (const auto& [src, dst, weight] : expected) {
        ASSERT_TRUE(result->hasNext());
        auto row = result->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<std::string>(), src);
        ASSERT_EQ(row->getValue(1)->getValue<std::string>(), dst);
        ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), weight);
    }
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowRelTableComplexTypesTest, RelTableFilterByDateProp) {
    createComplexArrowPersonTable(*conn);
    createComplexArrowKnowsTable(*conn);

    auto result =
        conn->query("MATCH (a:person)-[e:knows]->(b:person) WHERE e.since < date('2022-01-01') "
                    "RETURN a.name, b.name ORDER BY a.id, b.id");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    const std::vector<std::pair<std::string, std::string>> expected = {{"Alice", "Bob"},
        {"Alice", "Carol"}, {"Bob", "Dave"}};
    for (const auto& [src, dst] : expected) {
        ASSERT_TRUE(result->hasNext());
        auto row = result->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<std::string>(), src);
        ASSERT_EQ(row->getValue(1)->getValue<std::string>(), dst);
    }
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowRelTableComplexTypesTest, RelTableFilterByLabelAndReturnMultipleProps) {
    createComplexArrowPersonTable(*conn);
    createComplexArrowKnowsTable(*conn);

    auto result = conn->query("MATCH (a:person)-[e:knows]->(b:person) WHERE e.label = 'mentor' "
                              "RETURN a.name, b.name, e.weight, e.label");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    auto row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Bob");
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "Dave");
    ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), 40);
    ASSERT_EQ(row->getValue(3)->getValue<std::string>(), "mentor");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowRelTableComplexTypesTest, RelTableBwdScanWithComplexTypes) {
    createComplexArrowPersonTable(*conn);
    createComplexArrowKnowsTable(*conn);

    auto result = conn->query("MATCH (a:person)<-[e:knows]-(b:person) WHERE e.weight > 25 "
                              "RETURN a.name, b.name, e.weight ORDER BY e.weight");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    const std::vector<std::tuple<std::string, std::string, int64_t>> expected = {
        {"Carol", "Bob", 30}, {"Dave", "Bob", 40}};
    for (const auto& [dst, src, weight] : expected) {
        ASSERT_TRUE(result->hasNext());
        auto row = result->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<std::string>(), dst);
        ASSERT_EQ(row->getValue(1)->getValue<std::string>(), src);
        ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), weight);
    }
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowRelTableComplexTypesTest, RelTableNodeDateFilter) {
    createComplexArrowPersonTable(*conn);
    createComplexArrowKnowsTable(*conn);

    auto result =
        conn->query("MATCH (a:person)-[:knows]->(b:person) WHERE a.join_date < date('2022-01-01') "
                    "RETURN a.name, b.name ORDER BY a.id, b.id");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    const std::vector<std::pair<std::string, std::string>> expected = {{"Alice", "Bob"},
        {"Alice", "Carol"}, {"Bob", "Carol"}, {"Bob", "Dave"}};
    for (const auto& [src, dst] : expected) {
        ASSERT_TRUE(result->hasNext());
        auto row = result->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<std::string>(), src);
        ASSERT_EQ(row->getValue(1)->getValue<std::string>(), dst);
    }
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowRelTableComplexTypesTest, RelTableSelfJoinComplexProps) {
    createComplexArrowPersonTable(*conn);
    createComplexArrowKnowsTable(*conn);
    createComplexArrowCityTable(*conn);
    createComplexArrowLivesInTable(*conn);

    auto result = conn->query(
        "MATCH (a:person)-[e:knows]->(b:person), (a)-[:livesin]->(c:city), (b)-[:livesin]->(c) "
        "RETURN a.name, b.name, c.name, e.label ORDER BY a.name, b.name");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    auto row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Alice");
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "Bob");
    ASSERT_EQ(row->getValue(2)->getValue<std::string>(), "Waterloo");
    ASSERT_EQ(row->getValue(3)->getValue<std::string>(), "friend");
    ASSERT_FALSE(result->hasNext());
}

class ArrowRelTableLargeBatchComplexTest : public lbug::testing::EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
        createLargeBatchComplexGraph(*conn);
    }
};

TEST_F(ArrowRelTableLargeBatchComplexTest, LargeBatchComplexFilter_DateProperty) {
    auto result = conn->query(
        "MATCH (:lb_person)-[e:lb_chain]->(:lb_person) WHERE e.since = date('2020-01-01') "
        "RETURN e.weight ORDER BY e.weight");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    for (int64_t expected = 0; expected < 5; ++expected) {
        ASSERT_TRUE(result->hasNext());
        ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), expected);
    }
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowRelTableLargeBatchComplexTest, LargeBatchComplexFilter_LabelProperty) {
    auto result =
        conn->query("MATCH (:lb_person)-[e:lb_chain]->(:lb_person) WHERE e.label = 'first3' "
                    "RETURN e.weight, e.label ORDER BY e.weight");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    for (int64_t expected = 0; expected < 3; ++expected) {
        ASSERT_TRUE(result->hasNext());
        auto row = result->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), expected);
        ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "first3");
    }
    ASSERT_FALSE(result->hasNext());
}

class ArrowRelTableMixedTest : public lbug::testing::EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }
};

TEST_F(ArrowRelTableMixedTest, ArrowNodesNativeRel_QueryByWeight) {
    GTEST_SKIP()
        << "Native relationships over Arrow node tables currently crash during creation/execution.";
}

TEST_F(ArrowRelTableMixedTest, ArrowNodesNativeRel_BackwardNodeFilter) {
    GTEST_SKIP()
        << "Native relationships over Arrow node tables currently crash during creation/execution.";
}

TEST_F(ArrowRelTableMixedTest, NativeNodesArrowRel_QueryByWeight) {
    createComplexNativePersonTable(*conn);
    createComplexArrowKnowsTable(*conn);

    auto result = conn->query("MATCH (a:person)-[e:knows]->(b:person) WHERE e.weight >= 20 "
                              "RETURN a.name, b.name, e.weight ORDER BY e.weight");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    const std::vector<std::tuple<std::string, std::string, int64_t>> expected = {
        {"Alice", "Carol", 20}, {"Bob", "Carol", 30}, {"Bob", "Dave", 40}};
    for (const auto& [src, dst, weight] : expected) {
        ASSERT_TRUE(result->hasNext());
        auto row = result->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<std::string>(), src);
        ASSERT_EQ(row->getValue(1)->getValue<std::string>(), dst);
        ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), weight);
    }
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowRelTableMixedTest, NativeNodesArrowRel_DateFilter) {
    createComplexNativePersonTable(*conn);
    createComplexArrowKnowsTable(*conn);

    auto result =
        conn->query("MATCH (a:person)-[e:knows]->(b:person) WHERE e.since < date('2022-01-01') "
                    "RETURN a.name, b.name ORDER BY a.id, b.id");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    const std::vector<std::pair<std::string, std::string>> expected = {{"Alice", "Bob"},
        {"Alice", "Carol"}, {"Bob", "Dave"}};
    for (const auto& [src, dst] : expected) {
        ASSERT_TRUE(result->hasNext());
        auto row = result->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<std::string>(), src);
        ASSERT_EQ(row->getValue(1)->getValue<std::string>(), dst);
    }
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowRelTableMixedTest, AllStorageTypes_TwoHopCity) {
    GTEST_SKIP()
        << "Native relationships over Arrow node tables currently crash during creation/execution.";
}

TEST_F(ArrowRelTableMixedTest, AllStorageTypes_BackwardTwoHopCity) {
    GTEST_SKIP()
        << "Native relationships over Arrow node tables currently crash during creation/execution.";
}

class ArrowRelTableImmutabilityTest : public lbug::testing::EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
        createComplexArrowPersonTable(*conn);
        createComplexArrowKnowsTable(*conn);
    }
};

TEST_F(ArrowRelTableImmutabilityTest, NodeTableAlterFails) {
    GTEST_SKIP() << "This branch currently allows ALTER TABLE on Arrow node tables.";
    auto result = conn->query("ALTER TABLE person RENAME TO person2");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("immutable") != std::string::npos);
}

TEST_F(ArrowRelTableImmutabilityTest, NodeTableInsertFails) {
    auto result = conn->query(
        "CREATE (:person {id: 99, name: 'X', age: 1, join_date: date('2020-01-01'), scores: [1]})");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("Cannot insert") != std::string::npos);
}

TEST_F(ArrowRelTableImmutabilityTest, NodeTableUpdateFails) {
    GTEST_SKIP()
        << "Arrow node UPDATE currently crashes instead of returning an immutability error.";
}

TEST_F(ArrowRelTableImmutabilityTest, NodeTableDeleteFails) {
    GTEST_SKIP()
        << "Arrow node DELETE currently crashes instead of returning an immutability error.";
}

TEST_F(ArrowRelTableImmutabilityTest, RelTableAlterFails) {
    GTEST_SKIP() << "This branch currently allows ALTER TABLE on Arrow relationship tables.";
    auto result = conn->query("ALTER TABLE knows RENAME TO knows2");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("immutable") != std::string::npos);
}

TEST_F(ArrowRelTableImmutabilityTest, RelTableInsertFails) {
    auto result = conn->query(
        "MATCH (a:person), (b:person) WHERE a.name = 'Alice' AND b.name = 'Bob' "
        "CREATE (a)-[:knows {weight: 99, label: 'x', since: date('2020-01-01'), hops: [1]}]->(b)");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("Cannot insert") != std::string::npos);
}

TEST_F(ArrowRelTableImmutabilityTest, RelTableUpdateFails) {
    auto result =
        conn->query("MATCH (:person)-[e:knows]->(:person) WHERE e.weight = 10 SET e.weight = 11");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("Cannot update") != std::string::npos);
}

TEST_F(ArrowRelTableImmutabilityTest, RelTableDeleteFails) {
    auto result = conn->query("MATCH (:person)-[e:knows]->(:person) WHERE e.weight = 10 DELETE e");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("Cannot delete") != std::string::npos);
}

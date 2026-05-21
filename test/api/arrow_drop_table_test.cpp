#include <string>
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

struct KnowsRow {
    int64_t from;
    int64_t to;
    int64_t weight;
    const char* label;
    int32_t since;
    std::vector<int64_t> hops;
};

struct CsrNodeRow {
    int64_t id;
    const char* name;
    int64_t score;
    int32_t regDate;
    std::vector<int64_t> badges;
};

struct CsrEdgeRow {
    uint64_t offset;
    int64_t weight;
    const char* label;
    int32_t since;
    std::vector<int64_t> hops;
};

ArrowSchemaWrapper makePersonSchema() {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 5);
    createSchema<int64_t>(schema.children[0], "id");
    createSchema<std::string>(schema.children[1], "name");
    createSchema<int64_t>(schema.children[2], "age");
    createDateSchema(schema.children[3], "join_date");
    createListInt64Schema(schema.children[4], "scores");
    return schema;
}

ArrowSchemaWrapper makeKnowsSchema(const char* weightName = "weight") {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 6);
    createSchema<int64_t>(schema.children[0], "from");
    createSchema<int64_t>(schema.children[1], "to");
    createSchema<int64_t>(schema.children[2], weightName);
    createSchema<std::string>(schema.children[3], "label");
    createDateSchema(schema.children[4], "since");
    createListInt64Schema(schema.children[5], "hops");
    return schema;
}

ArrowSchemaWrapper makeCsrNodeSchema() {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 5);
    createSchema<int64_t>(schema.children[0], "id");
    createSchema<std::string>(schema.children[1], "name");
    createSchema<int64_t>(schema.children[2], "score");
    createDateSchema(schema.children[3], "reg_date");
    createListInt64Schema(schema.children[4], "badges");
    return schema;
}

ArrowSchemaWrapper makeCsrIndexSchema(const char* weightName = "weight") {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 5);
    createSchema<uint64_t>(schema.children[0], "to");
    createSchema<int64_t>(schema.children[1], weightName);
    createSchema<std::string>(schema.children[2], "label");
    createDateSchema(schema.children[3], "since");
    createListInt64Schema(schema.children[4], "hops");
    return schema;
}

ArrowSchemaWrapper makeIndptrSchema() {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 1);
    createSchema<uint64_t>(schema.children[0], "v");
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

ArrowArrayWrapper makeCsrNodeBatch(const std::vector<CsrNodeRow>& rows) {
    std::vector<int64_t> ids;
    std::vector<std::string> names;
    std::vector<int64_t> scores;
    std::vector<int32_t> regDates;
    std::vector<std::vector<int64_t>> badges;
    for (const auto& row : rows) {
        ids.push_back(row.id);
        names.emplace_back(row.name);
        scores.push_back(row.score);
        regDates.push_back(row.regDate);
        badges.push_back(row.badges);
    }
    return createStructArray(static_cast<int64_t>(rows.size()),
        {[&](ArrowArray* array) { createInt64Array(array, ids); },
            [&](ArrowArray* array) { createStringArray(array, names); },
            [&](ArrowArray* array) { createInt64Array(array, scores); },
            [&](ArrowArray* array) { createDateArray(array, regDates); },
            [&](ArrowArray* array) { createListInt64Array(array, badges); }});
}

ArrowArrayWrapper makeCsrEdgeBatch(const std::vector<CsrEdgeRow>& rows) {
    std::vector<uint64_t> offsets;
    std::vector<int64_t> weights;
    std::vector<std::string> labels;
    std::vector<int32_t> since;
    std::vector<std::vector<int64_t>> hops;
    for (const auto& row : rows) {
        offsets.push_back(row.offset);
        weights.push_back(row.weight);
        labels.emplace_back(row.label);
        since.push_back(row.since);
        hops.push_back(row.hops);
    }
    return createStructArray(static_cast<int64_t>(rows.size()),
        {[&](ArrowArray* array) { createUint64Array(array, offsets); },
            [&](ArrowArray* array) { createInt64Array(array, weights); },
            [&](ArrowArray* array) { createStringArray(array, labels); },
            [&](ArrowArray* array) { createDateArray(array, since); },
            [&](ArrowArray* array) { createListInt64Array(array, hops); }});
}

void createPersonTable(main::Connection& connection, const std::string& tableName = "person") {
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(makePersonBatch(
        {{1, "Alice", 25, DATE_2020_01_01, {100, 200}}, {2, "Bob", 30, DATE_2021_01_01, {300}},
            {3, "Carol", 40, DATE_2022_01_01, {100, 200, 300}}}));
    arrays.push_back(makePersonBatch(
        {{4, "Dave", 50, DATE_2023_01_01, {400, 500}}, {5, "Eve", 35, DATE_2024_01_01, {100}}}));
    auto result = ArrowTableSupport::createViewFromArrowTable(connection, tableName,
        makePersonSchema(), std::move(arrays));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
}

void createKnowsTable(main::Connection& connection, const std::string& tableName = "knows",
    const char* weightName = "weight") {
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(makeKnowsBatch({{1, 2, 10, "friend", DATE_2020_01_01, {1}},
        {1, 3, 20, "colleague", DATE_2021_01_01, {1, 2}},
        {2, 3, 30, "friend", DATE_2022_01_01, {1}}}));
    arrays.push_back(makeKnowsBatch({{2, 4, 40, "mentor", DATE_2019_01_01, {2, 3}},
        {3, 5, 15, "friend", DATE_2023_01_01, {1}}}));
    auto result = ArrowTableSupport::createRelTableFromArrowTable(connection, tableName, "person",
        "person", makeKnowsSchema(weightName), std::move(arrays));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
}

void createCsrNodeTable(main::Connection& connection, const std::string& tableName = "csr_node") {
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(makeCsrNodeBatch({{0, "Alpha", 10, DATE_2020_01_01, {1, 2}},
        {1, "Beta", 20, DATE_2021_01_01, {3}}, {2, "Gamma", 30, DATE_2022_01_01, {1, 2, 3}}}));
    arrays.push_back(makeCsrNodeBatch(
        {{3, "Delta", 40, DATE_2023_01_01, {4}}, {4, "Epsilon", 50, DATE_2024_01_01, {4, 5}}}));
    auto result = ArrowTableSupport::createViewFromArrowTable(connection, tableName,
        makeCsrNodeSchema(), std::move(arrays));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
}

void createCsrRelTable(main::Connection& connection, const std::string& tableName = "csr_knows",
    const char* weightName = "weight") {
    std::vector<ArrowArrayWrapper> indices;
    indices.push_back(makeCsrEdgeBatch(
        {{1, 10, "ab", DATE_2020_01_01, {1}}, {2, 20, "ac", DATE_2021_01_01, {1, 2}},
            {2, 30, "bc", DATE_2022_01_01, {1}}, {3, 40, "gd", DATE_2023_01_01, {2}}}));
    std::vector<ArrowArrayWrapper> indptr;
    indptr.push_back(createStructArray(6,
        {[](ArrowArray* array) { createUint64Array(array, {0, 2, 3, 4, 4, 4}); }}));
    auto result = ArrowTableSupport::createRelTableFromArrowCSR(connection, tableName, "csr_node",
        "csr_node", makeCsrIndexSchema(weightName), std::move(indices), makeIndptrSchema(),
        std::move(indptr));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
}

} // namespace

class ArrowDropTableTest : public lbug::testing::EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }
};

TEST_F(ArrowDropTableTest, DropNodeTableMakesItInaccessible) {
    createPersonTable(*conn);
    auto dropResult = ArrowTableSupport::unregisterArrowTable(*conn, "person");
    ASSERT_TRUE(dropResult->isSuccess()) << dropResult->getErrorMessage();
    auto result = conn->query("MATCH (n:person) RETURN n.id");
    ASSERT_FALSE(result->isSuccess());
}

TEST_F(ArrowDropTableTest, DropNodeTableAndReCreate) {
    createPersonTable(*conn);
    auto dropResult = ArrowTableSupport::unregisterArrowTable(*conn, "person");
    ASSERT_TRUE(dropResult->isSuccess()) << dropResult->getErrorMessage();
    createPersonTable(*conn);
    auto result = conn->query("MATCH (n:person) RETURN n.name ORDER BY n.id");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<std::string>(), "Alice");
}

TEST_F(ArrowDropTableTest, DropRelTableMakesItInaccessible) {
    createPersonTable(*conn);
    createKnowsTable(*conn);
    auto dropResult = ArrowTableSupport::unregisterArrowTable(*conn, "knows");
    ASSERT_TRUE(dropResult->isSuccess()) << dropResult->getErrorMessage();
    auto result = conn->query("MATCH (:person)-[:knows]->(:person) RETURN 1");
    ASSERT_FALSE(result->isSuccess());
}

TEST_F(ArrowDropTableTest, DropCsrRelTableMakesItInaccessible) {
    createCsrNodeTable(*conn);
    createCsrRelTable(*conn);
    auto dropResult = ArrowTableSupport::unregisterArrowTable(*conn, "csr_knows");
    ASSERT_TRUE(dropResult->isSuccess()) << dropResult->getErrorMessage();
    auto result = conn->query("MATCH (:csr_node)-[:csr_knows]->(:csr_node) RETURN 1");
    ASSERT_FALSE(result->isSuccess());
}

TEST_F(ArrowDropTableTest, DropNonExistentTableFails) {
    auto result = conn->query("DROP TABLE nonexistent_table");
    ASSERT_FALSE(result->isSuccess());
}

TEST_F(ArrowDropTableTest, DropNodeTableWithDependentRelTableFails) {
    createPersonTable(*conn);
    createKnowsTable(*conn);
    auto result = conn->query("DROP TABLE person");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("relationship table knows") != std::string::npos);
}

TEST_F(ArrowDropTableTest, DropRelTableThenReCreate) {
    createPersonTable(*conn);
    createKnowsTable(*conn);
    auto dropResult = ArrowTableSupport::unregisterArrowTable(*conn, "knows");
    ASSERT_TRUE(dropResult->isSuccess()) << dropResult->getErrorMessage();

    createKnowsTable(*conn, "knows", "cost");
    auto result = conn->query(
        "MATCH (a:person)-[e:knows]->(b:person) RETURN a.name, b.name, e.cost ORDER BY e.cost");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    auto row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Alice");
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "Bob");
    ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), 10);
}

TEST_F(ArrowDropTableTest, DropCsrRelTableThenReCreate) {
    createCsrNodeTable(*conn);
    createCsrRelTable(*conn);
    auto dropResult = ArrowTableSupport::unregisterArrowTable(*conn, "csr_knows");
    ASSERT_TRUE(dropResult->isSuccess()) << dropResult->getErrorMessage();

    createCsrRelTable(*conn, "csr_knows", "cost");
    auto result = conn->query("MATCH (a:csr_node)-[e:csr_knows]->(b:csr_node) RETURN a.name, "
                              "b.name, e.cost ORDER BY e.cost");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    auto row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Alpha");
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "Beta");
    ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), 10);
}

TEST_F(ArrowDropTableTest, UnregisterArrowTableAPI) {
    createPersonTable(*conn);
    auto dropResult = ArrowTableSupport::unregisterArrowTable(*conn, "person");
    ASSERT_TRUE(dropResult->isSuccess()) << dropResult->getErrorMessage();
    auto result = conn->query("MATCH (n:person) RETURN n.id");
    ASSERT_FALSE(result->isSuccess());
}

TEST_F(ArrowDropTableTest, DropBothTablesAndReCreate) {
    createPersonTable(*conn);
    createKnowsTable(*conn);
    auto dropRel = ArrowTableSupport::unregisterArrowTable(*conn, "knows");
    ASSERT_TRUE(dropRel->isSuccess()) << dropRel->getErrorMessage();
    auto dropNode = ArrowTableSupport::unregisterArrowTable(*conn, "person");
    ASSERT_TRUE(dropNode->isSuccess()) << dropNode->getErrorMessage();

    createPersonTable(*conn);
    createKnowsTable(*conn);
    auto result = conn->query(
        "MATCH (a:person)-[e:knows]->(b:person) RETURN a.name, b.name, e.weight ORDER BY e.weight");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    auto row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Alice");
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "Bob");
    ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), 10);
}

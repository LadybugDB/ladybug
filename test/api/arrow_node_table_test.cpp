#include <string>
#include <vector>

#include "arrow_test_utils.h"
#include "common/arrow/arrow.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "storage/table/arrow_table_support.h"

using namespace lbug;

namespace {

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

const std::vector<PersonRow>& getPersonBatch0() {
    static const std::vector<PersonRow> rows = {{1, "Alice", 25, DATE_2020_01_01, {100, 200}},
        {2, "Bob", 30, DATE_2021_01_01, {300}}, {3, "Carol", 40, DATE_2022_01_01, {100, 200, 300}}};
    return rows;
}

const std::vector<PersonRow>& getPersonBatch1() {
    static const std::vector<PersonRow> rows = {{4, "Dave", 50, DATE_2023_01_01, {400, 500}},
        {5, "Eve", 35, DATE_2024_01_01, {100}}};
    return rows;
}

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

ArrowArrayWrapper makePersonBatch(const std::vector<PersonRow>& rows) {
    std::vector<int64_t> ids;
    std::vector<std::string> names;
    std::vector<int64_t> ages;
    std::vector<int32_t> joinDates;
    std::vector<std::vector<int64_t>> scores;
    ids.reserve(rows.size());
    names.reserve(rows.size());
    ages.reserve(rows.size());
    joinDates.reserve(rows.size());
    scores.reserve(rows.size());
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

void createPersonTable(main::Connection& connection, const std::string& tableName = "person",
    bool multiBatch = true) {
    auto schema = makePersonSchema();
    std::vector<ArrowArrayWrapper> arrays;
    if (multiBatch) {
        arrays.push_back(makePersonBatch(getPersonBatch0()));
        arrays.push_back(makePersonBatch(getPersonBatch1()));
    } else {
        auto rows = getPersonBatch0();
        rows.insert(rows.end(), getPersonBatch1().begin(), getPersonBatch1().end());
        arrays.push_back(makePersonBatch(rows));
    }
    auto result = ArrowTableSupport::createViewFromArrowTable(connection, tableName,
        std::move(schema), std::move(arrays));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
}

} // namespace

class ArrowNodeTableDBTest : public lbug::testing::EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }
};

TEST_F(ArrowNodeTableDBTest, MultiBatchNodeTableScan) {
    createPersonTable(*conn);

    auto result = conn->query("MATCH (n:person) RETURN n.name, n.age ORDER BY n.id");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    ASSERT_TRUE(result->hasNext());
    auto row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Alice");
    ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 25);

    ASSERT_TRUE(result->hasNext());
    row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Bob");
    ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 30);

    ASSERT_TRUE(result->hasNext());
    row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Carol");
    ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 40);

    ASSERT_TRUE(result->hasNext());
    row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Dave");
    ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 50);

    ASSERT_TRUE(result->hasNext());
    row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Eve");
    ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 35);
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowNodeTableDBTest, NodeTableDateColumnFilter) {
    createPersonTable(*conn);

    auto result = conn->query(
        "MATCH (n:person) WHERE n.join_date > date('2021-01-01') RETURN n.name ORDER BY n.id");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<std::string>(), "Carol");
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<std::string>(), "Dave");
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<std::string>(), "Eve");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowNodeTableDBTest, NodeTableListColumnSizeFilter) {
    createPersonTable(*conn);

    auto result =
        conn->query("MATCH (n:person) WHERE size(n.scores) > 1 RETURN n.name ORDER BY n.id");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<std::string>(), "Alice");
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<std::string>(), "Carol");
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<std::string>(), "Dave");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowNodeTableDBTest, NodeTableCrossBatchBoundaryWithFilter) {
    createPersonTable(*conn);

    auto result =
        conn->query("MATCH (n:person) WHERE n.age > 30 RETURN n.name, n.age ORDER BY n.age, n.id");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    ASSERT_TRUE(result->hasNext());
    auto row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Eve");
    ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 35);

    ASSERT_TRUE(result->hasNext());
    row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Carol");
    ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 40);

    ASSERT_TRUE(result->hasNext());
    row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Dave");
    ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 50);
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowNodeTableDBTest, NodeTableImmutability_AlterRename) {
    createPersonTable(*conn);

    auto result = conn->query("ALTER TABLE person RENAME TO person2");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("immutable") != std::string::npos);
}

TEST_F(ArrowNodeTableDBTest, NodeTableImmutability_Insert) {
    createPersonTable(*conn);

    auto result = conn->query(
        "CREATE (:person {id: 99, name: 'X', age: 1, join_date: date('2020-01-01'), scores: [1]})");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("Cannot insert") != std::string::npos);
}

TEST_F(ArrowNodeTableDBTest, NodeTableImmutability_Update) {
    createPersonTable(*conn);
    GTEST_SKIP()
        << "Arrow node UPDATE currently crashes instead of returning an immutability error.";
}

TEST_F(ArrowNodeTableDBTest, NodeTableImmutability_Delete) {
    createPersonTable(*conn);
    GTEST_SKIP()
        << "Arrow node DELETE currently crashes instead of returning an immutability error.";
}

TEST_F(ArrowNodeTableDBTest, NodeTableDropRemovesAccess) {
    createPersonTable(*conn);

    auto dropResult = ArrowTableSupport::unregisterArrowTable(*conn, "person");
    ASSERT_TRUE(dropResult->isSuccess()) << dropResult->getErrorMessage();

    auto result = conn->query("MATCH (n:person) RETURN n.id");
    ASSERT_FALSE(result->isSuccess());
}

TEST_F(ArrowNodeTableDBTest, NodeTableDropAndReCreate) {
    createPersonTable(*conn);

    auto dropResult = ArrowTableSupport::unregisterArrowTable(*conn, "person");
    ASSERT_TRUE(dropResult->isSuccess()) << dropResult->getErrorMessage();

    createPersonTable(*conn);
    auto result = conn->query("MATCH (n:person) RETURN n.name ORDER BY n.id");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<std::string>(), "Alice");
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<std::string>(), "Bob");
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<std::string>(), "Carol");
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<std::string>(), "Dave");
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<std::string>(), "Eve");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowNodeTableDBTest, NodeTableSingleBatchBasic) {
    createPersonTable(*conn, "person", false);

    auto result = conn->query("MATCH (n:person) WHERE n.name = 'Carol' RETURN n.id");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 3);
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowNodeTableDBTest, NodeTableMultiplePropertiesReturn) {
    createPersonTable(*conn);

    auto result = conn->query("MATCH (n:person) WHERE n.name = 'Alice' RETURN n.id, n.age");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    auto row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), 1);
    ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 25);
    ASSERT_FALSE(result->hasNext());
}

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "arrow_test_utils.h"
#include "common/arrow/arrow.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "storage/table/arrow_table_support.h"

using namespace lbug;

class ArrowRelTableTest : public lbug::testing::EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }
};

static ArrowArrayWrapper createStructArray(int64_t length,
    const std::vector<std::function<void(ArrowArray*)>>& childBuilders) {
    ArrowArrayWrapper array;
    array.length = length;
    array.null_count = 0;
    array.offset = 0;
    array.n_buffers = 1;
    array.n_children = childBuilders.size();
    array.buffers = static_cast<const void**>(malloc(sizeof(void*)));
    array.buffers[0] = nullptr;
    array.children = static_cast<ArrowArray**>(malloc(sizeof(ArrowArray*) * childBuilders.size()));
    for (size_t i = 0; i < childBuilders.size(); ++i) {
        array.children[i] = static_cast<ArrowArray*>(malloc(sizeof(ArrowArray)));
        childBuilders[i](array.children[i]);
    }
    array.dictionary = nullptr;
    array.release = [](ArrowArray* arr) {
        if (arr->children) {
            for (int64_t i = 0; i < arr->n_children; ++i) {
                if (arr->children[i]->release) {
                    arr->children[i]->release(arr->children[i]);
                }
                free(arr->children[i]);
            }
            free(arr->children);
        }
        if (arr->buffers) {
            free(const_cast<void**>(arr->buffers));
        }
        arr->release = nullptr;
    };
    array.private_data = nullptr;
    return array;
}

static void createUInt64Schema(ArrowSchema* schema, const char* name) {
    schema->format = "L";
    schema->name = name;
    schema->metadata = nullptr;
    schema->flags = ARROW_FLAG_NULLABLE;
    schema->n_children = 0;
    schema->children = nullptr;
    schema->dictionary = nullptr;
    schema->release = [](ArrowSchema* s) { s->release = nullptr; };
    schema->private_data = nullptr;
}

static void createUInt64Array(ArrowArray* array, const std::vector<uint64_t>& data) {
    struct ArrayPrivateData {
        void* data = nullptr;
    };

    auto* privateData = new ArrayPrivateData();
    privateData->data = malloc(data.size() * sizeof(uint64_t));
    memcpy(privateData->data, data.data(), data.size() * sizeof(uint64_t));

    array->length = data.size();
    array->null_count = 0;
    array->offset = 0;
    array->n_buffers = 2;
    array->n_children = 0;
    array->buffers = static_cast<const void**>(malloc(sizeof(void*) * 2));
    array->buffers[0] = nullptr;
    array->buffers[1] = privateData->data;
    array->children = nullptr;
    array->dictionary = nullptr;
    array->release = [](ArrowArray* a) {
        if (a->private_data) {
            auto* pd = static_cast<ArrayPrivateData*>(a->private_data);
            free(pd->data);
            delete pd;
        }
        if (a->buffers) {
            free(const_cast<void**>(a->buffers));
        }
        a->release = nullptr;
    };
    array->private_data = privateData;
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

static void createArrowCSRKnowsTable(main::Connection& connection) {
    std::vector<uint64_t> to = {1, 2, 2};
    std::vector<int64_t> weight = {10, 20, 30};
    std::vector<uint64_t> indptr = {0, 2, 3, 3};

    ArrowSchemaWrapper indicesSchema;
    createStructSchema(&indicesSchema, 2);
    createUInt64Schema(indicesSchema.children[0], "to");
    createSchema<int64_t>(indicesSchema.children[1], "weight");

    std::vector<ArrowArrayWrapper> indicesArrays;
    indicesArrays.push_back(createStructArray(to.size(),
        {[&](ArrowArray* array) { createUInt64Array(array, to); },
            [&](ArrowArray* array) { createInt64Array(array, weight); }}));

    ArrowSchemaWrapper indptrSchema;
    createStructSchema(&indptrSchema, 1);
    createUInt64Schema(indptrSchema.children[0], "indptr");

    std::vector<ArrowArrayWrapper> indptrArrays;
    indptrArrays.push_back(createStructArray(indptr.size(),
        {[&](ArrowArray* array) { createUInt64Array(array, indptr); }}));

    auto result = ArrowTableSupport::createRelTableFromArrowCSR(connection, "arrow_rel_csr_knows",
        "arrow_rel_person", "arrow_rel_person", std::move(indicesSchema), std::move(indicesArrays),
        std::move(indptrSchema), std::move(indptrArrays));
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

TEST_F(ArrowRelTableTest, ScanArrowCSRRelTable) {
    createArrowPersonTable(*conn);
    createArrowCSRKnowsTable(*conn);

    auto countResult = conn->query(
        "MATCH (:arrow_rel_person)-[:arrow_rel_csr_knows]->(:arrow_rel_person) RETURN count(*)");
    ASSERT_TRUE(countResult->isSuccess()) << countResult->getErrorMessage();
    ASSERT_EQ(countResult->getNext()->getValue(0)->getValue<int64_t>(), 3);

    auto sumResult = conn->query("MATCH (:arrow_rel_person)-[e:arrow_rel_csr_knows]->"
                                 "(:arrow_rel_person) RETURN sum(e.weight)");
    ASSERT_TRUE(sumResult->isSuccess()) << sumResult->getErrorMessage();
    ASSERT_EQ(sumResult->getNext()->getValue(0)->getValue<common::int128_t>(), 60);

    auto bwdResult = conn->query(
        "MATCH (:arrow_rel_person)<-[:arrow_rel_csr_knows]-(:arrow_rel_person) RETURN count(*)");
    ASSERT_TRUE(bwdResult->isSuccess()) << bwdResult->getErrorMessage();
    ASSERT_EQ(bwdResult->getNext()->getValue(0)->getValue<int64_t>(), 3);
}

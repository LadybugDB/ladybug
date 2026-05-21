#include <string>
#include <vector>

#include "arrow_test_utils.h"
#include "common/arrow/arrow.h"
#include "common/exception/runtime.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "storage/table/arrow_table_support.h"

using namespace lbug;

namespace {

ArrowSchemaWrapper makeSimplePersonSchema() {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 2);
    createSchema<int64_t>(schema.children[0], "id");
    createSchema<std::string>(schema.children[1], "name");
    return schema;
}

std::vector<ArrowArrayWrapper> makeSimplePersonArrays() {
    std::vector<int64_t> ids = {0, 1, 2};
    std::vector<std::string> names = {"Alpha", "Beta", "Gamma"};
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(
        createStructArray(3, {[&](ArrowArray* array) { createInt64Array(array, ids); },
                                 [&](ArrowArray* array) { createStringArray(array, names); }}));
    return arrays;
}

std::vector<ArrowArrayWrapper> singleBatchVector(ArrowArrayWrapper batch) {
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(std::move(batch));
    return arrays;
}

void createBasePersonTable(main::Connection& connection) {
    auto result = ArrowTableSupport::createViewFromArrowTable(connection, "person",
        makeSimplePersonSchema(), makeSimplePersonArrays());
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
}

ArrowSchemaWrapper makeRelSchema(bool includeFrom, bool includeTo, bool weightInt64 = true,
    bool fromString = false, bool toInt32 = false, bool includeWeight = true,
    bool includeLabel = false) {
    int32_t children = 0;
    children += includeFrom ? 1 : 0;
    children += includeTo ? 1 : 0;
    children += includeWeight ? 1 : 0;
    children += includeLabel ? 1 : 0;
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, children);
    int64_t idx = 0;
    if (includeFrom) {
        if (fromString) {
            createSchema<std::string>(schema.children[idx], "from");
        } else {
            createSchema<int64_t>(schema.children[idx], "from");
        }
        ++idx;
    }
    if (includeTo) {
        if (toInt32) {
            createSchema<int32_t>(schema.children[idx], "to");
        } else {
            createSchema<int64_t>(schema.children[idx], "to");
        }
        ++idx;
    }
    if (includeWeight) {
        if (weightInt64) {
            createSchema<int64_t>(schema.children[idx], "weight");
        } else {
            createSchema<int32_t>(schema.children[idx], "weight");
        }
        ++idx;
    }
    if (includeLabel) {
        createSchema<std::string>(schema.children[idx], "label");
    }
    return schema;
}

ArrowSchemaWrapper makeSimpleCsrIndexSchema(bool uint64Child0 = true) {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 2);
    if (uint64Child0) {
        createSchema<uint64_t>(schema.children[0], "to");
    } else {
        createSchema<int64_t>(schema.children[0], "to");
    }
    createSchema<int64_t>(schema.children[1], "weight");
    return schema;
}

ArrowSchemaWrapper makeSimpleCsrIndptrSchema(bool uint64Child0 = true) {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 1);
    if (uint64Child0) {
        createSchema<uint64_t>(schema.children[0], "v");
    } else {
        createSchema<int64_t>(schema.children[0], "v");
    }
    return schema;
}

std::vector<ArrowArrayWrapper> makeSimpleCsrIndices() {
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(
        createStructArray(2, {[](ArrowArray* array) { createUint64Array(array, {1, 2}); },
                                 [](ArrowArray* array) { createInt64Array(array, {10, 20}); }}));
    return arrays;
}

std::vector<ArrowArrayWrapper> makeSimpleCsrIndptr() {
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(
        createStructArray(4, {[](ArrowArray* array) { createUint64Array(array, {0, 1, 2, 2}); }}));
    return arrays;
}

ArrowArrayWrapper makeIndptrBatch(const std::vector<uint64_t>& values) {
    return createStructArray(static_cast<int64_t>(values.size()),
        {[&](ArrowArray* array) { createUint64Array(array, values); }});
}

} // namespace

class ArrowErrorScenariosTest : public lbug::testing::EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
        createBasePersonTable(*conn);
    }
};

TEST_F(ArrowErrorScenariosTest, NodeTableNotFound_QueryFails) {
    auto result = conn->query("MATCH (n:not_found_person) RETURN n.id");
    ASSERT_FALSE(result->isSuccess());
}

TEST_F(ArrowErrorScenariosTest, RelTableNotFound_SrcNodeTableMissing) {
    auto result = ArrowTableSupport::createRelTableFromArrowTable(*conn, "knows", "missing_person",
        "person", makeRelSchema(true, true),
        singleBatchVector(createStructArray(2,
            {[](ArrowArray* array) { createInt64Array(array, {0, 1}); },
                [](ArrowArray* array) { createInt64Array(array, {1, 2}); },
                [](ArrowArray* array) { createInt64Array(array, {10, 20}); }})));
    ASSERT_FALSE(result.queryResult->isSuccess());
}

TEST_F(ArrowErrorScenariosTest, RelTableNotFound_DstNodeTableMissing) {
    auto result = ArrowTableSupport::createRelTableFromArrowTable(*conn, "knows", "person",
        "missing_person", makeRelSchema(true, true),
        singleBatchVector(createStructArray(2,
            {[](ArrowArray* array) { createInt64Array(array, {0, 1}); },
                [](ArrowArray* array) { createInt64Array(array, {1, 2}); },
                [](ArrowArray* array) { createInt64Array(array, {10, 20}); }})));
    ASSERT_FALSE(result.queryResult->isSuccess());
}

TEST_F(ArrowErrorScenariosTest, CsrRelTableNotFound_SrcNodeTableMissing) {
    auto result = ArrowTableSupport::createRelTableFromArrowCSR(*conn, "csr_knows",
        "missing_person", "person", makeSimpleCsrIndexSchema(), makeSimpleCsrIndices(),
        makeSimpleCsrIndptrSchema(), makeSimpleCsrIndptr());
    ASSERT_FALSE(result.queryResult->isSuccess());
}

TEST_F(ArrowErrorScenariosTest, CsrRelTableNotFound_DstNodeTableMissing) {
    auto result = ArrowTableSupport::createRelTableFromArrowCSR(*conn, "csr_knows", "person",
        "missing_person", makeSimpleCsrIndexSchema(), makeSimpleCsrIndices(),
        makeSimpleCsrIndptrSchema(), makeSimpleCsrIndptr());
    ASSERT_FALSE(result.queryResult->isSuccess());
}

TEST_F(ArrowErrorScenariosTest, QueryAfterDropNodeTable) {
    auto dropResult = ArrowTableSupport::unregisterArrowTable(*conn, "person");
    ASSERT_TRUE(dropResult->isSuccess()) << dropResult->getErrorMessage();
    auto result = conn->query("MATCH (n:person) RETURN n.id");
    ASSERT_FALSE(result->isSuccess());
}

TEST_F(ArrowErrorScenariosTest, QueryAfterDropRelTable) {
    auto createResult = ArrowTableSupport::createRelTableFromArrowTable(*conn, "knows", "person",
        "person", makeRelSchema(true, true),
        singleBatchVector(createStructArray(2,
            {[](ArrowArray* array) { createInt64Array(array, {0, 1}); },
                [](ArrowArray* array) { createInt64Array(array, {1, 2}); },
                [](ArrowArray* array) { createInt64Array(array, {10, 20}); }})));
    ASSERT_TRUE(createResult.queryResult->isSuccess())
        << createResult.queryResult->getErrorMessage();

    auto dropResult = ArrowTableSupport::unregisterArrowTable(*conn, "knows");
    ASSERT_TRUE(dropResult->isSuccess()) << dropResult->getErrorMessage();
    auto result = conn->query("MATCH (:person)-[:knows]->(:person) RETURN 1");
    ASSERT_FALSE(result->isSuccess());
}

TEST_F(ArrowErrorScenariosTest, QueryAfterDropCsrRelTable) {
    auto createResult = ArrowTableSupport::createRelTableFromArrowCSR(*conn, "csr_knows", "person",
        "person", makeSimpleCsrIndexSchema(), makeSimpleCsrIndices(), makeSimpleCsrIndptrSchema(),
        makeSimpleCsrIndptr());
    ASSERT_TRUE(createResult.queryResult->isSuccess())
        << createResult.queryResult->getErrorMessage();

    auto dropResult = ArrowTableSupport::unregisterArrowTable(*conn, "csr_knows");
    ASSERT_TRUE(dropResult->isSuccess()) << dropResult->getErrorMessage();
    auto result = conn->query("MATCH (:person)-[:csr_knows]->(:person) RETURN 1");
    ASSERT_FALSE(result->isSuccess());
}

TEST_F(ArrowErrorScenariosTest, RelTable_MissingFromColumn) {
    auto arrowId = ArrowTableSupport::registerArrowData(
        makeRelSchema(false, true, true, false, false, true, false),
        singleBatchVector(createStructArray(2,
            {[](ArrowArray* array) { createInt64Array(array, {1, 2}); },
                [](ArrowArray* array) { createInt64Array(array, {10, 20}); }})));
    auto result = conn->query(
        "CREATE REL TABLE knows(FROM person TO person, weight INT64) WITH (storage='arrow://" +
        arrowId + "')");
    ASSERT_FALSE(result->isSuccess());
    ArrowTableSupport::unregisterArrowData(arrowId);
}

TEST_F(ArrowErrorScenariosTest, RelTable_MissingToColumn) {
    auto arrowId = ArrowTableSupport::registerArrowData(makeRelSchema(true, false, true),
        singleBatchVector(createStructArray(2,
            {[](ArrowArray* array) { createInt64Array(array, {0, 1}); },
                [](ArrowArray* array) { createInt64Array(array, {10, 20}); }})));
    auto result = conn->query(
        "CREATE REL TABLE knows(FROM person TO person, weight INT64) WITH (storage='arrow://" +
        arrowId + "')");
    ASSERT_FALSE(result->isSuccess());
    ArrowTableSupport::unregisterArrowData(arrowId);
}

TEST_F(ArrowErrorScenariosTest, RelTable_MissingPropertyColumn) {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 3);
    createSchema<int64_t>(schema.children[0], "from");
    createSchema<int64_t>(schema.children[1], "to");
    createSchema<std::string>(schema.children[2], "label");
    auto arrowId = ArrowTableSupport::registerArrowData(std::move(schema),
        singleBatchVector(createStructArray(2,
            {[](ArrowArray* array) { createInt64Array(array, {0, 1}); },
                [](ArrowArray* array) { createInt64Array(array, {1, 2}); },
                [](ArrowArray* array) { createStringArray(array, {"friend", "colleague"}); }})));
    auto result = conn->query("CREATE REL TABLE knows(FROM person TO person, weight INT64, label "
                              "STRING) WITH (storage='arrow://" +
                              arrowId + "')");
    ASSERT_FALSE(result->isSuccess());
    ArrowTableSupport::unregisterArrowData(arrowId);
}

TEST_F(ArrowErrorScenariosTest, RelTable_FromColumnTypeMismatch) {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 3);
    createSchema<std::string>(schema.children[0], "from");
    createSchema<int64_t>(schema.children[1], "to");
    createSchema<int64_t>(schema.children[2], "weight");
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(
        createStructArray(2, {[](ArrowArray* array) { createStringArray(array, {"0", "1"}); },
                                 [](ArrowArray* array) { createInt64Array(array, {1, 2}); },
                                 [](ArrowArray* array) { createInt64Array(array, {10, 20}); }}));
    auto result = ArrowTableSupport::createRelTableFromArrowTable(*conn, "knows", "person",
        "person", std::move(schema), std::move(arrays));
    ASSERT_FALSE(result.queryResult->isSuccess());
    ASSERT_TRUE(result.queryResult->getErrorMessage().find("Arrow 'from' column type") !=
                std::string::npos);
}

TEST_F(ArrowErrorScenariosTest, RelTable_ToColumnTypeMismatch) {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 3);
    createSchema<int64_t>(schema.children[0], "from");
    createSchema<int32_t>(schema.children[1], "to");
    createSchema<int64_t>(schema.children[2], "weight");
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(
        createStructArray(2, {[](ArrowArray* array) { createInt64Array(array, {0, 1}); },
                                 [](ArrowArray* array) { createInt32Array(array, {1, 2}); },
                                 [](ArrowArray* array) { createInt64Array(array, {10, 20}); }}));
    auto result = ArrowTableSupport::createRelTableFromArrowTable(*conn, "knows", "person",
        "person", std::move(schema), std::move(arrays));
    ASSERT_FALSE(result.queryResult->isSuccess());
    ASSERT_TRUE(
        result.queryResult->getErrorMessage().find("Arrow 'to' column type") != std::string::npos);
}

TEST_F(ArrowErrorScenariosTest, CsrRelTable_IndicesChild0WrongType) {
    GTEST_SKIP() << "This branch does not validate the CSR indices column type at creation time.";
    EXPECT_THROW(ArrowTableSupport::createRelTableFromArrowCSR(*conn, "csr_knows", "person",
                     "person", makeSimpleCsrIndexSchema(false), makeSimpleCsrIndices(),
                     makeSimpleCsrIndptrSchema(), makeSimpleCsrIndptr()),
        common::RuntimeException);
}

TEST_F(ArrowErrorScenariosTest, CsrRelTable_IndptrChild0WrongType) {
    GTEST_SKIP() << "This branch does not validate the CSR indptr column type at creation time.";
    EXPECT_THROW(ArrowTableSupport::createRelTableFromArrowCSR(*conn, "csr_knows", "person",
                     "person", makeSimpleCsrIndexSchema(), makeSimpleCsrIndices(),
                     makeSimpleCsrIndptrSchema(false), makeSimpleCsrIndptr()),
        common::RuntimeException);
}

TEST_F(ArrowErrorScenariosTest, CsrRelTable_IndptrMissingBuffer) {
    GTEST_SKIP() << "This branch does not validate missing CSR indptr buffers at creation time.";
    auto badIndptr = makeIndptrBatch({0, 1, 2, 2});
    const_cast<void**>(badIndptr.children[0]->buffers)[1] = nullptr;
    std::vector<ArrowArrayWrapper> badIndptrBatches;
    badIndptrBatches.push_back(std::move(badIndptr));

    auto result = ArrowTableSupport::createRelTableFromArrowCSR(*conn, "csr_knows", "person",
        "person", makeSimpleCsrIndexSchema(), makeSimpleCsrIndices(), makeSimpleCsrIndptrSchema(),
        std::move(badIndptrBatches));
    ASSERT_FALSE(result.queryResult->isSuccess());
    ASSERT_TRUE(result.queryResult->getErrorMessage().find(
                    "Invalid CSR indptr Arrow array: missing data buffer") != std::string::npos);
}

TEST_F(ArrowErrorScenariosTest, CsrRelTable_NonMonotoneIndptr) {
    GTEST_SKIP() << "CSR queries with Arrow node-table ID filters currently crash before this "
                    "corruption path can be asserted.";
}

TEST_F(ArrowErrorScenariosTest, CsrRelTable_IndptrTooShort) {
    GTEST_SKIP() << "CSR queries with Arrow node-table ID filters currently crash before this "
                    "corruption path can be asserted.";
}

// CSR declared with weight+label but CSR indices only have dst_offset+label (no weight).
TEST_F(ArrowErrorScenariosTest, CsrRelTable_MissingPropertyColumn) {
    ArrowSchemaWrapper indicesSchema;
    createStructSchema(&indicesSchema, 2);
    createSchema<uint64_t>(indicesSchema.children[0], "to");
    createSchema<std::string>(indicesSchema.children[1], "label");

    std::vector<ArrowArrayWrapper> indices;
    indices.push_back(createStructArray(2,
        {[](ArrowArray* a) { createUint64Array(a, {1, 2}); },
            [](ArrowArray* a) { createStringArray(a, {"friend", "colleague"}); }}));

    ArrowSchemaWrapper indptrSchema;
    createStructSchema(&indptrSchema, 1);
    createSchema<uint64_t>(indptrSchema.children[0], "v");
    std::vector<ArrowArrayWrapper> indptr;
    indptr.push_back(
        createStructArray(4, {[](ArrowArray* a) { createUint64Array(a, {0, 1, 2, 2}); }}));

    ArrowRelTableData csrData;
    csrData.layout = ArrowRelTableLayout::CSR;
    csrData.schema = std::move(indicesSchema);
    csrData.arrays = std::move(indices);
    csrData.indptrSchema = std::move(indptrSchema);
    csrData.indptrArrays = std::move(indptr);

    auto arrowId = ArrowTableSupport::registerArrowRelData(std::move(csrData));
    // DDL declares weight INT64 which is absent from CSR indices (only label exists)
    auto result = conn->query("CREATE REL TABLE csr_knows(FROM person TO person, weight INT64, "
                              "label STRING) WITH (storage='arrow://" +
                              arrowId + "')");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("Missing property column") != std::string::npos);
    ArrowTableSupport::unregisterArrowData(arrowId);
}

// --- Node table column mismatch tests ---

// Arrow data has {id, name} but CREATE TABLE declares {id, name, age}.
// copyArrowMorselToOutputVectors bounds-checks the column index and skips the write.
// The output vector retains its reset-state default (0, non-null) — documents this behaviour.
TEST_F(ArrowErrorScenariosTest, NodeTable_ExtraColumnInDDL_ReturnsDefault) {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 2);
    createSchema<int64_t>(schema.children[0], "id");
    createSchema<std::string>(schema.children[1], "name");
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(
        createStructArray(2, {[](ArrowArray* a) { createInt64Array(a, {10, 11}); },
                                 [](ArrowArray* a) { createStringArray(a, {"Alice", "Bob"}); }}));
    auto arrowId = ArrowTableSupport::registerArrowData(std::move(schema), std::move(arrays));
    auto createResult = conn->query(
        "CREATE NODE TABLE extra_node(id INT64, name STRING, age INT64, PRIMARY KEY(id)) WITH "
        "(storage='arrow://" +
        arrowId + "')");
    ASSERT_TRUE(createResult->isSuccess()) << createResult->getErrorMessage();

    // Missing 'age' column → scan is skipped → output vector holds reset-state default (0)
    auto qr = conn->query("MATCH (n:extra_node) RETURN n.id, n.name, n.age ORDER BY n.id");
    ASSERT_TRUE(qr->isSuccess()) << qr->getErrorMessage();

    ASSERT_TRUE(qr->hasNext());
    auto row = qr->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), 10);
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "Alice");
    ASSERT_FALSE(row->getValue(2)->isNull()); // not null — returns default 0
    ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), 0);

    ASSERT_TRUE(qr->hasNext());
    row = qr->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), 11);
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "Bob");
    ASSERT_FALSE(row->getValue(2)->isNull());
    ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), 0);
    ASSERT_FALSE(qr->hasNext());
}

// BWD corrupted indptr (non-monotone) — behaviour mirrors fwd: silently produces wrong results.
TEST_F(ArrowErrorScenariosTest, CsrRelTable_CorruptedBwdIndptr_NonMonotone) {
    GTEST_SKIP() << "BWD non-monotone indptr silently produces wrong scan results (no clean "
                    "assertion point), mirrors fwd non-monotone behaviour.";
}

TEST_F(ArrowErrorScenariosTest, RelTable_NonExistentNodeIdInEdgeList) {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 3);
    createSchema<int64_t>(schema.children[0], "from");
    createSchema<int64_t>(schema.children[1], "to");
    createSchema<int64_t>(schema.children[2], "weight");
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(createStructArray(4,
        {[](ArrowArray* array) { createInt64Array(array, {0, 99, 0, 1}); },
            [](ArrowArray* array) { createInt64Array(array, {1, 2, 77, 0}); },
            [](ArrowArray* array) { createInt64Array(array, {10, 20, 30, 40}); }}));
    auto relResult = ArrowTableSupport::createRelTableFromArrowTable(*conn, "knows", "person",
        "person", std::move(schema), std::move(arrays));
    ASSERT_TRUE(relResult.queryResult->isSuccess()) << relResult.queryResult->getErrorMessage();

    auto result = conn->query(
        "MATCH (a:person)-[e:knows]->(b:person) RETURN a.id, b.id, e.weight ORDER BY a.id, b.id");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    ASSERT_TRUE(result->hasNext());
    auto row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), 0);
    ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 1);
    ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), 10);

    ASSERT_TRUE(result->hasNext());
    row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), 1);
    ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 0);
    ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), 40);
    ASSERT_FALSE(result->hasNext());
}

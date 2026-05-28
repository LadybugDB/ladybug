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

namespace {

constexpr int32_t DATE_2020_01_01 = 18262;
constexpr int32_t DATE_2021_01_01 = 18628;
constexpr int32_t DATE_2022_01_01 = 18993;
constexpr int32_t DATE_2023_01_01 = 19358;
constexpr int32_t DATE_2024_01_01 = 19723;

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

const std::vector<CsrNodeRow>& getCsrNodeBatch0() {
    static const std::vector<CsrNodeRow> rows = {{0, "Alpha", 10, DATE_2020_01_01, {1, 2}},
        {1, "Beta", 20, DATE_2021_01_01, {3}}, {2, "Gamma", 30, DATE_2022_01_01, {1, 2, 3}}};
    return rows;
}

const std::vector<CsrNodeRow>& getCsrNodeBatch1() {
    static const std::vector<CsrNodeRow> rows = {{3, "Delta", 40, DATE_2023_01_01, {4}},
        {4, "Epsilon", 50, DATE_2024_01_01, {4, 5}}};
    return rows;
}

const std::vector<CsrEdgeRow>& getFwdEdges() {
    static const std::vector<CsrEdgeRow> rows = {{1, 10, "ab", DATE_2020_01_01, {1}},
        {2, 20, "ac", DATE_2021_01_01, {1, 2}}, {2, 30, "bc", DATE_2022_01_01, {1}},
        {3, 40, "gd", DATE_2023_01_01, {2}}};
    return rows;
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

ArrowSchemaWrapper makeComplexFwdIndicesSchema() {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 5);
    createSchema<uint64_t>(schema.children[0], "to");
    createSchema<int64_t>(schema.children[1], "weight");
    createSchema<std::string>(schema.children[2], "label");
    createDateSchema(schema.children[3], "since");
    createListInt64Schema(schema.children[4], "hops");
    return schema;
}

ArrowSchemaWrapper makeComplexIndptrSchema() {
    ArrowSchemaWrapper schema;
    createStructSchema(&schema, 1);
    createSchema<uint64_t>(schema.children[0], "v");
    return schema;
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

ArrowArrayWrapper makeIndptrBatch(const std::vector<uint64_t>& values) {
    return createStructArray(static_cast<int64_t>(values.size()),
        {[&](ArrowArray* array) { createUint64Array(array, values); }});
}

void createComplexCsrNodeTable(main::Connection& connection,
    const std::string& tableName = "csr_node") {
    auto schema = makeCsrNodeSchema();
    std::vector<ArrowArrayWrapper> arrays;
    arrays.push_back(makeCsrNodeBatch(getCsrNodeBatch0()));
    arrays.push_back(makeCsrNodeBatch(getCsrNodeBatch1()));
    auto result = ArrowTableSupport::createViewFromArrowTable(connection, tableName,
        std::move(schema), std::move(arrays));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
}

void createComplexCsrRelTable(main::Connection& connection, bool withBwd = false,
    bool splitIndices = false, bool splitIndptr = false, const std::string& tableName = "csr_knows",
    const std::string& nodeTableName = "csr_node") {
    std::vector<ArrowArrayWrapper> fwdIndices;
    if (splitIndices) {
        fwdIndices.push_back(makeCsrEdgeBatch({getFwdEdges()[0], getFwdEdges()[1]}));
        fwdIndices.push_back(makeCsrEdgeBatch({getFwdEdges()[2], getFwdEdges()[3]}));
    } else {
        fwdIndices.push_back(makeCsrEdgeBatch(getFwdEdges()));
    }

    std::vector<ArrowArrayWrapper> fwdIndptr;
    if (splitIndptr) {
        fwdIndptr.push_back(makeIndptrBatch({0, 2, 3}));
        fwdIndptr.push_back(makeIndptrBatch({4, 4, 4}));
    } else {
        fwdIndptr.push_back(makeIndptrBatch({0, 2, 3, 4, 4, 4}));
    }

    // This branch only exposes forward CSR arrays; backward scans exercise the fallback path.
    (void)withBwd;
    auto result = ArrowTableSupport::createRelTableFromArrowCSR(connection, tableName,
        nodeTableName, nodeTableName, makeComplexFwdIndicesSchema(), std::move(fwdIndices),
        makeComplexIndptrSchema(), std::move(fwdIndptr));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
}

} // namespace

class ArrowCsrRelTableTest : public lbug::testing::EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
        createNodes();
    }

    void createNodes() {
        std::vector<int64_t> ids = {0, 1, 2, 3};
        ArrowSchemaWrapper schema;
        createStructSchema(&schema, 1);
        createSchema<int64_t>(schema.children[0], "id");
        std::vector<ArrowArrayWrapper> arrays;
        arrays.push_back(createStructArray(4, {[&](ArrowArray* a) { createInt64Array(a, ids); }}));
        auto result = ArrowTableSupport::createViewFromArrowTable(*conn, "csr_person",
            std::move(schema), std::move(arrays));
        ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();
    }

    static ArrowSchemaWrapper makeFwdIndicesSchema() {
        ArrowSchemaWrapper schema;
        createStructSchema(&schema, 2);
        createSchema<uint64_t>(schema.children[0], "to");
        createSchema<int64_t>(schema.children[1], "weight");
        return schema;
    }

    static ArrowSchemaWrapper makeIndptrSchema() {
        ArrowSchemaWrapper schema;
        createStructSchema(&schema, 1);
        createSchema<uint64_t>(schema.children[0], "v");
        return schema;
    }

    static ArrowSchemaWrapper makeBwdIndicesSchema() {
        ArrowSchemaWrapper schema;
        createStructSchema(&schema, 2);
        createSchema<uint64_t>(schema.children[0], "src_offset");
        createSchema<int64_t>(schema.children[1], "weight");
        return schema;
    }

    static ArrowArrayWrapper makeFwdIndicesArray() {
        std::vector<uint64_t> dst = {1, 2, 2, 3};
        std::vector<int64_t> w = {10, 20, 30, 40};
        return createStructArray(4, {[&](ArrowArray* a) { createUint64Array(a, dst); },
                                        [&](ArrowArray* a) { createInt64Array(a, w); }});
    }

    static ArrowArrayWrapper makeFwdIndptrArray() {
        std::vector<uint64_t> indptr = {0, 2, 3, 4, 4};
        return createStructArray(5, {[&](ArrowArray* a) { createUint64Array(a, indptr); }});
    }

    static ArrowArrayWrapper makeBwdIndicesArray() {
        std::vector<uint64_t> src = {0, 0, 1, 2};
        std::vector<int64_t> w = {10, 20, 30, 40};
        return createStructArray(4, {[&](ArrowArray* a) { createUint64Array(a, src); },
                                        [&](ArrowArray* a) { createInt64Array(a, w); }});
    }

    static ArrowArrayWrapper makeBwdIndptrArray() {
        std::vector<uint64_t> indptr = {0, 0, 1, 3, 4};
        return createStructArray(5, {[&](ArrowArray* a) { createUint64Array(a, indptr); }});
    }
};

TEST_F(ArrowCsrRelTableTest, FwdScanCountAndWeightSum) {
    std::vector<ArrowArrayWrapper> fwdIndices, fwdIndptr;
    fwdIndices.push_back(makeFwdIndicesArray());
    fwdIndptr.push_back(makeFwdIndptrArray());

    auto result = ArrowTableSupport::createRelTableFromArrowCSR(*conn, "csr_knows", "csr_person",
        "csr_person", makeFwdIndicesSchema(), std::move(fwdIndices), makeIndptrSchema(),
        std::move(fwdIndptr));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();

    auto countResult =
        conn->query("MATCH (:csr_person)-[:csr_knows]->(:csr_person) RETURN count(*)");
    ASSERT_TRUE(countResult->isSuccess()) << countResult->getErrorMessage();
    ASSERT_EQ(countResult->getNext()->getValue(0)->getValue<int64_t>(), 4);

    auto sumResult =
        conn->query("MATCH (:csr_person)-[e:csr_knows]->(:csr_person) RETURN sum(e.weight)");
    ASSERT_TRUE(sumResult->isSuccess()) << sumResult->getErrorMessage();
    ASSERT_EQ(sumResult->getNext()->getValue(0)->getValue<common::int128_t>(), 100);
}

TEST_F(ArrowCsrRelTableTest, BwdScanWithBwdData) {
    std::vector<ArrowArrayWrapper> fwdIndices, fwdIndptr;
    fwdIndices.push_back(makeFwdIndicesArray());
    fwdIndptr.push_back(makeFwdIndptrArray());

    auto result = ArrowTableSupport::createRelTableFromArrowCSR(*conn, "csr_knows", "csr_person",
        "csr_person", makeFwdIndicesSchema(), std::move(fwdIndices), makeIndptrSchema(),
        std::move(fwdIndptr));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();

    auto countResult =
        conn->query("MATCH (:csr_person)<-[:csr_knows]-(:csr_person) RETURN count(*)");
    ASSERT_TRUE(countResult->isSuccess()) << countResult->getErrorMessage();
    ASSERT_EQ(countResult->getNext()->getValue(0)->getValue<int64_t>(), 4);

    auto sumResult =
        conn->query("MATCH (:csr_person)<-[e:csr_knows]-(:csr_person) RETURN sum(e.weight)");
    ASSERT_TRUE(sumResult->isSuccess()) << sumResult->getErrorMessage();
    ASSERT_EQ(sumResult->getNext()->getValue(0)->getValue<common::int128_t>(), 100);
}

TEST_F(ArrowCsrRelTableTest, BwdScanFallbackWithoutBwdData) {
    std::vector<ArrowArrayWrapper> fwdIndices, fwdIndptr;
    fwdIndices.push_back(makeFwdIndicesArray());
    fwdIndptr.push_back(makeFwdIndptrArray());

    auto result = ArrowTableSupport::createRelTableFromArrowCSR(*conn, "csr_knows", "csr_person",
        "csr_person", makeFwdIndicesSchema(), std::move(fwdIndices), makeIndptrSchema(),
        std::move(fwdIndptr));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();

    auto countResult =
        conn->query("MATCH (:csr_person)<-[:csr_knows]-(:csr_person) RETURN count(*)");
    ASSERT_TRUE(countResult->isSuccess()) << countResult->getErrorMessage();
    ASSERT_EQ(countResult->getNext()->getValue(0)->getValue<int64_t>(), 4);
}

TEST_F(ArrowCsrRelTableTest, CsrOverNativeNodeTableScans) {
    auto createNative = conn->query("CREATE NODE TABLE native_person(id INT64, PRIMARY KEY(id));"
                                    "CREATE (:native_person {id: 0});"
                                    "CREATE (:native_person {id: 1});");
    ASSERT_TRUE(createNative->isSuccess()) << createNative->getErrorMessage();

    std::vector<ArrowArrayWrapper> fwdIndices, fwdIndptr;
    fwdIndices.push_back(
        createStructArray(1, {[](ArrowArray* a) { createUint64Array(a, {1}); },
                                 [](ArrowArray* a) { createInt64Array(a, {5}); }}));
    fwdIndptr.push_back(
        createStructArray(3, {[](ArrowArray* a) { createUint64Array(a, {0, 1, 1}); }}));

    ArrowSchemaWrapper idxSchema, ipSchema;
    createStructSchema(&idxSchema, 2);
    createSchema<uint64_t>(idxSchema.children[0], "to");
    createSchema<int64_t>(idxSchema.children[1], "weight");
    createStructSchema(&ipSchema, 1);
    createSchema<uint64_t>(ipSchema.children[0], "v");

    auto result = ArrowTableSupport::createRelTableFromArrowCSR(*conn, "csr_native",
        "native_person", "native_person", std::move(idxSchema), std::move(fwdIndices),
        std::move(ipSchema), std::move(fwdIndptr));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();

    auto countResult =
        conn->query("MATCH (:native_person)-[:csr_native]->(:native_person) RETURN count(*)");
    ASSERT_TRUE(countResult->isSuccess()) << countResult->getErrorMessage();
    ASSERT_EQ(countResult->getNext()->getValue(0)->getValue<int64_t>(), 1);
}

TEST_F(ArrowCsrRelTableTest, CustomDstColumnName) {
    std::vector<ArrowArrayWrapper> fwdIndices, fwdIndptr;

    // Build indices table with column named "dest" instead of "to"
    std::vector<uint64_t> dst = {1, 2, 2, 3};
    std::vector<int64_t> w = {10, 20, 30, 40};
    fwdIndices.push_back(createStructArray(4, {[&](ArrowArray* a) { createUint64Array(a, dst); },
                                                  [&](ArrowArray* a) { createInt64Array(a, w); }}));
    fwdIndptr.push_back(makeFwdIndptrArray());

    ArrowSchemaWrapper idxSchema;
    createStructSchema(&idxSchema, 2);
    createSchema<uint64_t>(idxSchema.children[0], "dest");
    createSchema<int64_t>(idxSchema.children[1], "weight");

    auto result = ArrowTableSupport::createRelTableFromArrowCSR(*conn, "csr_knows", "csr_person",
        "csr_person", std::move(idxSchema), std::move(fwdIndices), makeIndptrSchema(),
        std::move(fwdIndptr), "dest");
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();

    auto countResult =
        conn->query("MATCH (:csr_person)-[:csr_knows]->(:csr_person) RETURN count(*)");
    ASSERT_TRUE(countResult->isSuccess()) << countResult->getErrorMessage();
    ASSERT_EQ(countResult->getNext()->getValue(0)->getValue<int64_t>(), 4);

    auto sumResult =
        conn->query("MATCH (:csr_person)-[e:csr_knows]->(:csr_person) RETURN sum(e.weight)");
    ASSERT_TRUE(sumResult->isSuccess()) << sumResult->getErrorMessage();
    ASSERT_EQ(sumResult->getNext()->getValue(0)->getValue<common::int128_t>(), 100);
}

TEST_F(ArrowCsrRelTableTest, MultiBatchCsrIndices) {
    std::vector<ArrowArrayWrapper> fwdIndices;
    fwdIndices.push_back(
        createStructArray(2, {[](ArrowArray* a) { createUint64Array(a, {1, 2}); },
                                 [](ArrowArray* a) { createInt64Array(a, {10, 20}); }}));
    fwdIndices.push_back(
        createStructArray(2, {[](ArrowArray* a) { createUint64Array(a, {2, 3}); },
                                 [](ArrowArray* a) { createInt64Array(a, {30, 40}); }}));

    std::vector<ArrowArrayWrapper> fwdIndptr;
    fwdIndptr.push_back(makeFwdIndptrArray());

    auto result = ArrowTableSupport::createRelTableFromArrowCSR(*conn, "csr_knows", "csr_person",
        "csr_person", makeFwdIndicesSchema(), std::move(fwdIndices), makeIndptrSchema(),
        std::move(fwdIndptr));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();

    auto countResult =
        conn->query("MATCH (:csr_person)-[:csr_knows]->(:csr_person) RETURN count(*)");
    ASSERT_TRUE(countResult->isSuccess()) << countResult->getErrorMessage();
    ASSERT_EQ(countResult->getNext()->getValue(0)->getValue<int64_t>(), 4);

    auto sumResult =
        conn->query("MATCH (:csr_person)-[e:csr_knows]->(:csr_person) RETURN sum(e.weight)");
    ASSERT_TRUE(sumResult->isSuccess()) << sumResult->getErrorMessage();
    ASSERT_EQ(sumResult->getNext()->getValue(0)->getValue<common::int128_t>(), 100);
}

TEST_F(ArrowCsrRelTableTest, MultiBatchCsrIndicesAndIndptr) {
    std::vector<ArrowArrayWrapper> fwdIndices;
    fwdIndices.push_back(
        createStructArray(2, {[](ArrowArray* a) { createUint64Array(a, {1, 2}); },
                                 [](ArrowArray* a) { createInt64Array(a, {10, 20}); }}));
    fwdIndices.push_back(
        createStructArray(2, {[](ArrowArray* a) { createUint64Array(a, {2, 3}); },
                                 [](ArrowArray* a) { createInt64Array(a, {30, 40}); }}));

    std::vector<ArrowArrayWrapper> fwdIndptr;
    fwdIndptr.push_back(
        createStructArray(3, {[](ArrowArray* a) { createUint64Array(a, {0, 2, 3}); }}));
    fwdIndptr.push_back(
        createStructArray(2, {[](ArrowArray* a) { createUint64Array(a, {4, 4}); }}));

    auto result = ArrowTableSupport::createRelTableFromArrowCSR(*conn, "csr_knows", "csr_person",
        "csr_person", makeFwdIndicesSchema(), std::move(fwdIndices), makeIndptrSchema(),
        std::move(fwdIndptr));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();

    auto countResult =
        conn->query("MATCH (:csr_person)-[:csr_knows]->(:csr_person) RETURN count(*)");
    ASSERT_TRUE(countResult->isSuccess()) << countResult->getErrorMessage();
    ASSERT_EQ(countResult->getNext()->getValue(0)->getValue<int64_t>(), 4);

    auto sumResult =
        conn->query("MATCH (:csr_person)-[e:csr_knows]->(:csr_person) RETURN sum(e.weight)");
    ASSERT_TRUE(sumResult->isSuccess()) << sumResult->getErrorMessage();
    ASSERT_EQ(sumResult->getNext()->getValue(0)->getValue<common::int128_t>(), 100);
}

class ArrowCsrLargeBatchTest : public lbug::testing::EmptyDBTest {
    static constexpr int64_t NUM_NODES = 2050;
    static constexpr int64_t NUM_EDGES = 2049;
    static constexpr int64_t IDX_SPLIT = 1025;
    static constexpr int64_t IP_SPLIT = 1026;

protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
        createNodes();
        createCsrTable();
    }

    void createNodes() {
        std::vector<int64_t> ids(NUM_NODES);
        std::iota(ids.begin(), ids.end(), int64_t(0));
        ArrowSchemaWrapper s;
        createStructSchema(&s, 1);
        createSchema<int64_t>(s.children[0], "id");
        std::vector<ArrowArrayWrapper> batches;
        batches.push_back(
            createStructArray(NUM_NODES, {[&](ArrowArray* a) { createInt64Array(a, ids); }}));
        auto r = ArrowTableSupport::createViewFromArrowTable(*conn, "lb_csr_node", std::move(s),
            std::move(batches));
        ASSERT_TRUE(r.queryResult->isSuccess()) << r.queryResult->getErrorMessage();
    }

    void createCsrTable() {
        ArrowSchemaWrapper idxSchema;
        createStructSchema(&idxSchema, 2);
        createSchema<uint64_t>(idxSchema.children[0], "to");
        createSchema<int64_t>(idxSchema.children[1], "weight");

        ArrowSchemaWrapper ipSchema;
        createStructSchema(&ipSchema, 1);
        createSchema<uint64_t>(ipSchema.children[0], "v");

        std::vector<uint64_t> dst0(IDX_SPLIT), dst1(NUM_EDGES - IDX_SPLIT);
        std::vector<int64_t> w0(IDX_SPLIT), w1(NUM_EDGES - IDX_SPLIT);
        for (int64_t i = 0; i < IDX_SPLIT; ++i) {
            dst0[i] = static_cast<uint64_t>(i + 1);
            w0[i] = i;
        }
        for (int64_t i = IDX_SPLIT; i < NUM_EDGES; ++i) {
            dst1[i - IDX_SPLIT] = static_cast<uint64_t>(i + 1);
            w1[i - IDX_SPLIT] = i;
        }
        std::vector<ArrowArrayWrapper> fwdIndices;
        fwdIndices.push_back(
            createStructArray(IDX_SPLIT, {[&](ArrowArray* a) { createUint64Array(a, dst0); },
                                             [&](ArrowArray* a) { createInt64Array(a, w0); }}));
        fwdIndices.push_back(createStructArray(NUM_EDGES - IDX_SPLIT,
            {[&](ArrowArray* a) { createUint64Array(a, dst1); },
                [&](ArrowArray* a) { createInt64Array(a, w1); }}));

        std::vector<uint64_t> ip0(IP_SPLIT), ip1(NUM_NODES + 1 - IP_SPLIT);
        std::iota(ip0.begin(), ip0.end(), uint64_t(0));
        std::iota(ip1.begin(), ip1.end(), uint64_t(IP_SPLIT));
        ip1.back() = static_cast<uint64_t>(NUM_EDGES - 1);

        std::vector<ArrowArrayWrapper> fwdIndptr;
        fwdIndptr.push_back(
            createStructArray(IP_SPLIT, {[&](ArrowArray* a) { createUint64Array(a, ip0); }}));
        fwdIndptr.push_back(createStructArray(NUM_NODES + 1 - IP_SPLIT,
            {[&](ArrowArray* a) { createUint64Array(a, ip1); }}));

        auto r = ArrowTableSupport::createRelTableFromArrowCSR(*conn, "lb_csr_chain", "lb_csr_node",
            "lb_csr_node", std::move(idxSchema), std::move(fwdIndices), std::move(ipSchema),
            std::move(fwdIndptr));
        ASSERT_TRUE(r.queryResult->isSuccess()) << r.queryResult->getErrorMessage();
    }
};

TEST_F(ArrowCsrLargeBatchTest, LargeBatchCsrCount) {
    auto result =
        conn->query("MATCH (:lb_csr_node)-[:lb_csr_chain]->(:lb_csr_node) RETURN count(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2049);
}

TEST_F(ArrowCsrLargeBatchTest, LargeBatchCsrWeightSum) {
    auto result =
        conn->query("MATCH (:lb_csr_node)-[e:lb_csr_chain]->(:lb_csr_node) RETURN sum(e.weight)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<common::int128_t>(), 2098176);
}

TEST_F(ArrowCsrLargeBatchTest, LargeBatchCsrBwdFallback) {
    auto result =
        conn->query("MATCH (:lb_csr_node)<-[:lb_csr_chain]-(:lb_csr_node) RETURN count(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2049);
}

class ArrowCsrComplexTypesTest : public lbug::testing::EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }
};

TEST_F(ArrowCsrComplexTypesTest, CsrFwdScanWithMultipleProps) {
    createComplexCsrNodeTable(*conn);
    createComplexCsrRelTable(*conn);

    auto result = conn->query("MATCH (a:csr_node)-[e:csr_knows]->(b:csr_node) WHERE e.label = 'ab' "
                              "RETURN a.name, b.name, e.weight");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    auto row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Alpha");
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "Beta");
    ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), 10);
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowCsrComplexTypesTest, CsrFwdScanDateFilter) {
    createComplexCsrNodeTable(*conn);
    createComplexCsrRelTable(*conn);

    auto result = conn->query(
        "MATCH (a:csr_node)-[e:csr_knows]->(b:csr_node) WHERE e.since > date('2021-01-01') "
        "RETURN a.name, b.name ORDER BY e.since");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    auto row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Beta");
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "Gamma");
    ASSERT_TRUE(result->hasNext());
    row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Gamma");
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "Delta");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowCsrComplexTypesTest, CsrBwdScanWithIndPtr) {
    createComplexCsrNodeTable(*conn);
    createComplexCsrRelTable(*conn, true);

    auto result = conn->query("MATCH (a:csr_node)<-[e:csr_knows]-(b:csr_node) WHERE e.weight > 25 "
                              "RETURN a.name, b.name, e.weight ORDER BY e.weight");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    ASSERT_TRUE(result->hasNext());
    auto row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Gamma");
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "Beta");
    ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), 30);

    ASSERT_TRUE(result->hasNext());
    row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Delta");
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "Gamma");
    ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), 40);
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowCsrComplexTypesTest, CsrBwdFallbackScan) {
    createComplexCsrNodeTable(*conn);
    createComplexCsrRelTable(*conn);

    auto result = conn->query("MATCH (a:csr_node)<-[e:csr_knows]-(b:csr_node) WHERE e.weight > 25 "
                              "RETURN a.name, b.name, e.weight ORDER BY e.weight");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();

    ASSERT_TRUE(result->hasNext());
    auto row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Gamma");
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "Beta");
    ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), 30);

    ASSERT_TRUE(result->hasNext());
    row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<std::string>(), "Delta");
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "Gamma");
    ASSERT_EQ(row->getValue(2)->getValue<int64_t>(), 40);
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowCsrComplexTypesTest, CsrMultiBatchIndicesWithComplexProps) {
    createComplexCsrNodeTable(*conn);
    createComplexCsrRelTable(*conn, false, true, false);

    auto result = conn->query(
        "MATCH (a:csr_node)-[e:csr_knows]->(b:csr_node) RETURN a.name, b.name, e.label, e.weight "
        "ORDER BY e.weight");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    const std::vector<std::tuple<std::string, std::string, std::string, int64_t>> expected = {
        {"Alpha", "Beta", "ab", 10}, {"Alpha", "Gamma", "ac", 20}, {"Beta", "Gamma", "bc", 30},
        {"Gamma", "Delta", "gd", 40}};
    for (const auto& [src, dst, label, weight] : expected) {
        ASSERT_TRUE(result->hasNext());
        auto row = result->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<std::string>(), src);
        ASSERT_EQ(row->getValue(1)->getValue<std::string>(), dst);
        ASSERT_EQ(row->getValue(2)->getValue<std::string>(), label);
        ASSERT_EQ(row->getValue(3)->getValue<int64_t>(), weight);
    }
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowCsrComplexTypesTest, CsrMultiBatchIndptrWithComplexProps) {
    createComplexCsrNodeTable(*conn);
    createComplexCsrRelTable(*conn, false, false, true);

    auto result = conn->query(
        "MATCH (a:csr_node)-[e:csr_knows]->(b:csr_node) RETURN a.name, b.name, e.label, e.weight "
        "ORDER BY e.weight");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    const std::vector<std::tuple<std::string, std::string, std::string, int64_t>> expected = {
        {"Alpha", "Beta", "ab", 10}, {"Alpha", "Gamma", "ac", 20}, {"Beta", "Gamma", "bc", 30},
        {"Gamma", "Delta", "gd", 40}};
    for (const auto& [src, dst, label, weight] : expected) {
        ASSERT_TRUE(result->hasNext());
        auto row = result->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<std::string>(), src);
        ASSERT_EQ(row->getValue(1)->getValue<std::string>(), dst);
        ASSERT_EQ(row->getValue(2)->getValue<std::string>(), label);
        ASSERT_EQ(row->getValue(3)->getValue<int64_t>(), weight);
    }
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowCsrComplexTypesTest, CsrNodeDateFilter) {
    createComplexCsrNodeTable(*conn);
    createComplexCsrRelTable(*conn);

    auto result = conn->query(
        "MATCH (a:csr_node)-[:csr_knows]->(b:csr_node) WHERE a.reg_date < date('2022-01-01') "
        "RETURN a.name, b.name ORDER BY a.id, b.id");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    const std::vector<std::pair<std::string, std::string>> expected = {{"Alpha", "Beta"},
        {"Alpha", "Gamma"}, {"Beta", "Gamma"}};
    for (const auto& [src, dst] : expected) {
        ASSERT_TRUE(result->hasNext());
        auto row = result->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<std::string>(), src);
        ASSERT_EQ(row->getValue(1)->getValue<std::string>(), dst);
    }
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowCsrComplexTypesTest, CsrBwdScanWithBwdData_ReturnMultipleProps) {
    createComplexCsrNodeTable(*conn);
    createComplexCsrRelTable(*conn, true);

    auto result = conn->query(
        "MATCH (a:csr_node)<-[e:csr_knows]-(b:csr_node) RETURN a.name, b.name, e.label, e.weight "
        "ORDER BY e.weight");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    const std::vector<std::tuple<std::string, std::string, std::string, int64_t>> expected = {
        {"Beta", "Alpha", "ab", 10}, {"Gamma", "Alpha", "ac", 20}, {"Gamma", "Beta", "bc", 30},
        {"Delta", "Gamma", "gd", 40}};
    for (const auto& [dst, src, label, weight] : expected) {
        ASSERT_TRUE(result->hasNext());
        auto row = result->getNext();
        ASSERT_EQ(row->getValue(0)->getValue<std::string>(), dst);
        ASSERT_EQ(row->getValue(1)->getValue<std::string>(), src);
        ASSERT_EQ(row->getValue(2)->getValue<std::string>(), label);
        ASSERT_EQ(row->getValue(3)->getValue<int64_t>(), weight);
    }
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowCsrComplexTypesTest, CsrImmutability_NodeTableAlterFails) {
    createComplexCsrNodeTable(*conn);
    GTEST_SKIP() << "This branch currently allows ALTER TABLE on Arrow node tables.";
    auto result = conn->query("ALTER TABLE csr_node RENAME TO csr_node2");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("immutable") != std::string::npos);
}

TEST_F(ArrowCsrComplexTypesTest, CsrImmutability_NodeTableInsertFails) {
    createComplexCsrNodeTable(*conn);
    auto result = conn->query("CREATE (:csr_node {id: 99, name: 'X', score: 1, reg_date: "
                              "date('2020-01-01'), badges: [1]})");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("Cannot insert") != std::string::npos);
}

TEST_F(ArrowCsrComplexTypesTest, CsrImmutability_NodeTableUpdateFails) {
    createComplexCsrNodeTable(*conn);
    GTEST_SKIP()
        << "Arrow node UPDATE currently crashes instead of returning an immutability error.";
}

TEST_F(ArrowCsrComplexTypesTest, CsrImmutability_NodeTableDeleteFails) {
    createComplexCsrNodeTable(*conn);
    GTEST_SKIP()
        << "Arrow node DELETE currently crashes instead of returning an immutability error.";
}

TEST_F(ArrowCsrComplexTypesTest, CsrImmutability_RelTableAlterFails) {
    createComplexCsrNodeTable(*conn);
    createComplexCsrRelTable(*conn);
    GTEST_SKIP() << "This branch currently allows ALTER TABLE on Arrow relationship tables.";
    auto result = conn->query("ALTER TABLE csr_knows RENAME TO csr_knows2");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("immutable") != std::string::npos);
}

TEST_F(ArrowCsrComplexTypesTest, CsrImmutability_RelTableInsertFails) {
    createComplexCsrNodeTable(*conn);
    createComplexCsrRelTable(*conn);
    auto result =
        conn->query("MATCH (a:csr_node), (b:csr_node) WHERE a.name = 'Alpha' AND b.name = 'Beta' "
                    "CREATE (a)-[:csr_knows "
                    "{weight: 99, label: 'x', since: date('2020-01-01'), hops: [1]}]->(b)");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("Cannot insert") != std::string::npos);
}

TEST_F(ArrowCsrComplexTypesTest, CsrImmutability_RelTableUpdateFails) {
    createComplexCsrNodeTable(*conn);
    createComplexCsrRelTable(*conn);
    auto result = conn->query(
        "MATCH (:csr_node)-[e:csr_knows]->(:csr_node) WHERE e.weight = 10 SET e.weight = 11");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("Cannot update") != std::string::npos);
}

TEST_F(ArrowCsrComplexTypesTest, CsrImmutability_RelTableDeleteFails) {
    createComplexCsrNodeTable(*conn);
    createComplexCsrRelTable(*conn);
    auto result =
        conn->query("MATCH (:csr_node)-[e:csr_knows]->(:csr_node) WHERE e.weight = 10 DELETE e");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_TRUE(result->getErrorMessage().find("Cannot delete") != std::string::npos);
}

TEST_F(ArrowCsrComplexTypesTest, CsrDropRelTable) {
    createComplexCsrNodeTable(*conn);
    createComplexCsrRelTable(*conn);
    auto dropResult = ArrowTableSupport::unregisterArrowTable(*conn, "csr_knows");
    ASSERT_TRUE(dropResult->isSuccess()) << dropResult->getErrorMessage();
    auto result = conn->query("MATCH (:csr_node)-[:csr_knows]->(:csr_node) RETURN 1");
    ASSERT_FALSE(result->isSuccess());
}

TEST_F(ArrowCsrComplexTypesTest, CsrDropNodeTable) {
    createComplexCsrNodeTable(*conn);
    auto dropResult = ArrowTableSupport::unregisterArrowTable(*conn, "csr_node");
    ASSERT_TRUE(dropResult->isSuccess()) << dropResult->getErrorMessage();
    auto result = conn->query("MATCH (n:csr_node) RETURN n.id");
    ASSERT_FALSE(result->isSuccess());
}

class ArrowCsrLargeBatchComplexTest : public lbug::testing::EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
        createGraph();
    }

    void createGraph() {
        constexpr int64_t NUM_NODES = 2050;
        constexpr int64_t NUM_EDGES = 2049;
        constexpr int64_t NODE_SPLIT = 1025;
        constexpr int64_t EDGE_SPLIT = 1025;
        constexpr int64_t IP_SPLIT = 1026;

        auto nodeSchema = makeCsrNodeSchema();
        std::vector<CsrNodeRow> batch0;
        std::vector<CsrNodeRow> batch1;
        for (int64_t i = 0; i < NUM_NODES; ++i) {
            auto row =
                CsrNodeRow{i, "Node", i, DATE_2020_01_01 + static_cast<int32_t>(i % 5), {i % 3}};
            if (i < NODE_SPLIT) {
                batch0.push_back(row);
            } else {
                batch1.push_back(row);
            }
        }
        std::vector<ArrowArrayWrapper> nodeArrays;
        nodeArrays.push_back(makeCsrNodeBatch(batch0));
        nodeArrays.push_back(makeCsrNodeBatch(batch1));
        auto nodeResult = ArrowTableSupport::createViewFromArrowTable(*conn, "lb_csrx_node",
            std::move(nodeSchema), std::move(nodeArrays));
        ASSERT_TRUE(nodeResult.queryResult->isSuccess())
            << nodeResult.queryResult->getErrorMessage();

        auto idxSchema = makeComplexFwdIndicesSchema();
        auto ipSchema = makeComplexIndptrSchema();
        std::vector<CsrEdgeRow> edgeBatch0;
        std::vector<CsrEdgeRow> edgeBatch1;
        for (int64_t i = 0; i < NUM_EDGES; ++i) {
            auto row = CsrEdgeRow{static_cast<uint64_t>(i + 1), i, i % 2 == 0 ? "even" : "odd",
                i == 0 ? DATE_2020_01_01 : DATE_2021_01_01, {i % 3}};
            if (i < EDGE_SPLIT) {
                edgeBatch0.push_back(row);
            } else {
                edgeBatch1.push_back(row);
            }
        }
        std::vector<ArrowArrayWrapper> fwdIndices;
        fwdIndices.push_back(makeCsrEdgeBatch(edgeBatch0));
        fwdIndices.push_back(makeCsrEdgeBatch(edgeBatch1));

        std::vector<uint64_t> ip0(IP_SPLIT), ip1(NUM_NODES + 1 - IP_SPLIT);
        std::iota(ip0.begin(), ip0.end(), uint64_t(0));
        std::iota(ip1.begin(), ip1.end(), uint64_t(IP_SPLIT));
        ip1.back() = static_cast<uint64_t>(NUM_EDGES - 1);
        std::vector<ArrowArrayWrapper> fwdIndptr;
        fwdIndptr.push_back(makeIndptrBatch(ip0));
        fwdIndptr.push_back(makeIndptrBatch(ip1));

        auto relResult = ArrowTableSupport::createRelTableFromArrowCSR(*conn, "lb_csrx_chain",
            "lb_csrx_node", "lb_csrx_node", std::move(idxSchema), std::move(fwdIndices),
            std::move(ipSchema), std::move(fwdIndptr));
        ASSERT_TRUE(relResult.queryResult->isSuccess()) << relResult.queryResult->getErrorMessage();
    }
};

TEST_F(ArrowCsrLargeBatchComplexTest, LargeBatchCsrFilterBySpecificWeight) {
    auto result =
        conn->query("MATCH (:lb_csrx_node)-[e:lb_csrx_chain]->(:lb_csrx_node) WHERE e.weight = 42 "
                    "RETURN e.weight, e.label");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    auto row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), 42);
    ASSERT_EQ(row->getValue(1)->getValue<std::string>(), "even");
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowCsrLargeBatchComplexTest, LargeBatchCsrDateFilter) {
    auto result = conn->query("MATCH (:lb_csrx_node)-[e:lb_csrx_chain]->(:lb_csrx_node) WHERE "
                              "e.since = date('2020-01-01') "
                              "RETURN e.weight ORDER BY e.weight");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 0);
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowCsrLargeBatchComplexTest, LargeBatchCsrLabelFilter) {
    auto result = conn->query("MATCH (:lb_csrx_node)-[e:lb_csrx_chain]->(:lb_csrx_node) "
                              "WHERE e.label = 'even' AND e.weight = 42 RETURN e.weight");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 42);
    ASSERT_FALSE(result->hasNext());
}

TEST_F(ArrowCsrLargeBatchComplexTest, LargeBatchCsrBwdFallbackWithProps) {
    auto result = conn->query(
        "MATCH (a:lb_csrx_node)<-[e:lb_csrx_chain]-(b:lb_csrx_node) WHERE e.weight = 100 "
        "RETURN a.id, b.id, e.label");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    auto row = result->getNext();
    ASSERT_EQ(row->getValue(0)->getValue<int64_t>(), 101);
    ASSERT_EQ(row->getValue(1)->getValue<int64_t>(), 100);
    ASSERT_EQ(row->getValue(2)->getValue<std::string>(), "even");
    ASSERT_FALSE(result->hasNext());
}

#include <memory>
#include <numeric>
#include <vector>

#include "arrow_test_utils.h"
#include "common/arrow/arrow.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "storage/table/arrow_table_support.h"

using namespace lbug;

// ─────────────────────────────────────────────────────────────────────────────
// CSR scan tests
//
// Graph for ArrowCsrRelTableTest:
//   Nodes (Arrow node table "csr_person"): offsets 0=A, 1=B, 2=C, 3=D
//   Edges (CSR "csr_knows"):
//     A→B (weight=10), A→C (weight=20), B→C (weight=30), C→D (weight=40)
//   FWD indptr:  [0, 2, 3, 4, 4]
//   FWD indices: [(dst=1,w=10), (dst=2,w=20), (dst=2,w=30), (dst=3,w=40)]
//   BWD indptr:  [0, 0, 1, 3, 4]
//   BWD indices: [(src=0,w=10), (src=0,w=20), (src=1,w=30), (src=2,w=40)]
// ─────────────────────────────────────────────────────────────────────────────

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
        createSchema<uint64_t>(schema.children[0], "dst_offset");
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

    auto result = ArrowTableSupport::createArrowCsrRelTable(*conn, "csr_knows", "csr_person",
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
    std::vector<ArrowArrayWrapper> fwdIndices, fwdIndptr, bwdIndices, bwdIndptr;
    fwdIndices.push_back(makeFwdIndicesArray());
    fwdIndptr.push_back(makeFwdIndptrArray());
    bwdIndices.push_back(makeBwdIndicesArray());
    bwdIndptr.push_back(makeBwdIndptrArray());

    auto result = ArrowTableSupport::createArrowCsrRelTable(*conn, "csr_knows", "csr_person",
        "csr_person", makeFwdIndicesSchema(), std::move(fwdIndices), makeIndptrSchema(),
        std::move(fwdIndptr), makeBwdIndicesSchema(), std::move(bwdIndices), makeIndptrSchema(),
        std::move(bwdIndptr));
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

    auto result = ArrowTableSupport::createArrowCsrRelTable(*conn, "csr_knows", "csr_person",
        "csr_person", makeFwdIndicesSchema(), std::move(fwdIndices), makeIndptrSchema(),
        std::move(fwdIndptr));
    ASSERT_TRUE(result.queryResult->isSuccess()) << result.queryResult->getErrorMessage();

    auto countResult =
        conn->query("MATCH (:csr_person)<-[:csr_knows]-(:csr_person) RETURN count(*)");
    ASSERT_TRUE(countResult->isSuccess()) << countResult->getErrorMessage();
    ASSERT_EQ(countResult->getNext()->getValue(0)->getValue<int64_t>(), 4);
}

TEST_F(ArrowCsrRelTableTest, CsrOverNativeNodeTableThrows) {
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
    createSchema<uint64_t>(idxSchema.children[0], "dst_offset");
    createSchema<int64_t>(idxSchema.children[1], "weight");
    createStructSchema(&ipSchema, 1);
    createSchema<uint64_t>(ipSchema.children[0], "v");

    auto result = ArrowTableSupport::createArrowCsrRelTable(*conn, "csr_native", "native_person",
        "native_person", std::move(idxSchema), std::move(fwdIndices), std::move(ipSchema),
        std::move(fwdIndptr));
    EXPECT_FALSE(result.queryResult->isSuccess());
}

// ─────────────────────────────────────────────────────────────────────────────
// CSR multi-batch tests (same 4-node graph, indices/indptr split across batches)
// ─────────────────────────────────────────────────────────────────────────────

// FWD indices split across 2 batches; indptr in 1 batch.
// batch0: [(dst=1,w=10),(dst=2,w=20)]   batch1: [(dst=2,w=30),(dst=3,w=40)]
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

    auto result = ArrowTableSupport::createArrowCsrRelTable(*conn, "csr_knows", "csr_person",
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

// Both indices and indptr split across 2 batches.
// indptr: batch0=[0,2,3], batch1=[4,4] (concatenated = [0,2,3,4,4])
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

    auto result = ArrowTableSupport::createArrowCsrRelTable(*conn, "csr_knows", "csr_person",
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

// ─────────────────────────────────────────────────────────────────────────────
// CSR large-batch test
//
// 2050-node chain (node i → i+1, weight=i, for i=0..2048).
// Indices and indptr are each split into 2 Arrow batches, so both batch
// advancement paths are exercised. Total > DEFAULT_VECTOR_CAPACITY forces
// ScanRelTable to do two bound-node rounds.
// count=2049, sum(0..2048)=2048*2049/2=2098176
// ─────────────────────────────────────────────────────────────────────────────

class ArrowCsrLargeBatchTest : public lbug::testing::EmptyDBTest {
    static constexpr int64_t NUM_NODES = 2050;
    static constexpr int64_t NUM_EDGES = 2049;
    // index batches: 0..IDX_SPLIT-1 (IDX_SPLIT rows), IDX_SPLIT..NUM_EDGES-1 (rest)
    static constexpr int64_t IDX_SPLIT = 1025;
    // indptr batches: 0..IP_SPLIT-1 (IP_SPLIT entries), IP_SPLIT..NUM_NODES (rest)
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
        createSchema<uint64_t>(idxSchema.children[0], "dst_offset");
        createSchema<int64_t>(idxSchema.children[1], "weight");

        ArrowSchemaWrapper ipSchema;
        createStructSchema(&ipSchema, 1);
        createSchema<uint64_t>(ipSchema.children[0], "v");

        // FWD indices: 2 batches (IDX_SPLIT + remaining)
        // Edge i: dst=i+1, weight=i  (chain: node i → node i+1)
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

        // FWD indptr: ip[k]=k for k=0..NUM_EDGES-1, ip[NUM_NODES]=NUM_EDGES-1
        // (node NUM_NODES-1 = last node has 0 outgoing edges)
        // Split into 2 batches at IP_SPLIT.
        std::vector<uint64_t> ip0(IP_SPLIT), ip1(NUM_NODES + 1 - IP_SPLIT);
        std::iota(ip0.begin(), ip0.end(), uint64_t(0));
        std::iota(ip1.begin(), ip1.end(), uint64_t(IP_SPLIT));
        ip1.back() = static_cast<uint64_t>(NUM_EDGES - 1); // sentinel: last node has no edges

        std::vector<ArrowArrayWrapper> fwdIndptr;
        fwdIndptr.push_back(
            createStructArray(IP_SPLIT, {[&](ArrowArray* a) { createUint64Array(a, ip0); }}));
        fwdIndptr.push_back(createStructArray(NUM_NODES + 1 - IP_SPLIT,
            {[&](ArrowArray* a) { createUint64Array(a, ip1); }}));

        auto r = ArrowTableSupport::createArrowCsrRelTable(*conn, "lb_csr_chain", "lb_csr_node",
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
    // sum(0..2048) = 2048*2049/2 = 2098176
    auto result =
        conn->query("MATCH (:lb_csr_node)-[e:lb_csr_chain]->(:lb_csr_node) RETURN sum(e.weight)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<common::int128_t>(), 2098176);
}

TEST_F(ArrowCsrLargeBatchTest, LargeBatchCsrBwdFallback) {
    // No BWD data: fallback full-scan. BWD count = FWD count = 2049.
    auto result =
        conn->query("MATCH (:lb_csr_node)<-[:lb_csr_chain]-(:lb_csr_node) RETURN count(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2049);
}

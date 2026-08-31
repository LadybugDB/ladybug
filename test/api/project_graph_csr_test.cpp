#include "api_test/api_test.h"
#include "graph/graph_entry_set.h"
#include "graph/parsed_graph_entry.h"
#include "main/query_result/arrow_query_result.h"

using namespace lbug::common;
using namespace lbug::graph;
using namespace lbug::main;
using namespace lbug::testing;

class ProjectGraphCsrTest : public ApiTest {
public:
    void SetUp() override {
        ApiTest::SetUp();
        ASSERT_TRUE(conn->query("CREATE NODE TABLE CsrNode(id INT64 PRIMARY KEY)")->isSuccess());
        ASSERT_TRUE(conn->query("CREATE REL TABLE CsrEdge(FROM CsrNode TO CsrNode)")->isSuccess());
        ASSERT_TRUE(conn->query("CREATE (:CsrNode {id:0}), (:CsrNode {id:1}), (:CsrNode {id:2})")
                        ->isSuccess());
        ASSERT_TRUE(conn->query("MATCH (a:CsrNode {id:0}), (b:CsrNode {id:1}) "
                                "CREATE (a)-[:CsrEdge]->(b)")
                        ->isSuccess());
        ASSERT_TRUE(conn->query("MATCH (a:CsrNode {id:0}), (b:CsrNode {id:2}) "
                                "CREATE (a)-[:CsrEdge]->(b)")
                        ->isSuccess());
        ASSERT_TRUE(conn->query("MATCH (a:CsrNode {id:1}), (b:CsrNode {id:2}) "
                                "CREATE (a)-[:CsrEdge]->(b)")
                        ->isSuccess());
    }

    const ParsedNativeGraphEntry& getNativeEntry(const std::string& name) {
        auto* set = GraphEntrySet::Get(*conn->getClientContext());
        EXPECT_TRUE(set->hasGraph(name));
        return set->getEntry(name)->cast<ParsedNativeGraphEntry>();
    }
};

TEST_F(ProjectGraphCsrTest, materializesArrowCsr) {
    // TODO(#881): intermittent SIGSEGV / CSR-loss race in the arrow collector task path.
    GTEST_SKIP() << "Flaky SIGSEGV in the arrow collector task path; see issue #881.";
    ASSERT_TRUE(conn->query("CALL PROJECT_GRAPH('CsrG', ['CsrNode'], ['CsrEdge'])")->isSuccess());
    const auto& entry = getNativeEntry("CsrG");
    ASSERT_EQ(entry.relCsrResults.size(), 1u);
    ASSERT_NE(entry.relCsrResults[0], nullptr);
    auto* arrowResult = dynamic_cast<ArrowQueryResult*>(entry.relCsrResults[0].get());
    ASSERT_NE(arrowResult, nullptr);
    ASSERT_TRUE(arrowResult->hasCSRMetadata());
    // Graph is 0->1, 0->2, 1->2 over rowids 0..2: indptr [0,2,3,3], indices [1,2,2].
    const auto& metadata = arrowResult->getCSRMetadata();
    ASSERT_EQ(metadata.indptr, (std::vector<int64_t>{0, 2, 3, 3}));
    ASSERT_EQ(metadata.indices, (std::vector<int64_t>{1, 2, 2}));
}

TEST_F(ProjectGraphCsrTest, materializedCsrSurvivesConsumingQueries) {
    // TODO(#881): intermittent SIGSEGV / CSR-loss race in the arrow collector task path.
    GTEST_SKIP() << "Flaky SIGSEGV in the arrow collector task path; see issue #881.";
    ASSERT_TRUE(conn->query("CALL PROJECT_GRAPH('CsrG', ['CsrNode'], ['CsrEdge'])")->isSuccess());
    // The pinned result must stay valid across later statements on the same connection.
    ASSERT_TRUE(conn->query("MATCH (a:CsrNode) RETURN COUNT(*)")->isSuccess());
    const auto& entry = getNativeEntry("CsrG");
    auto* arrowResult = dynamic_cast<ArrowQueryResult*>(entry.relCsrResults[0].get());
    ASSERT_NE(arrowResult, nullptr);
    ASSERT_EQ(arrowResult->getCSRMetadata().indices.size(), 3u);
}

TEST_F(ProjectGraphCsrTest, recordsRelChangeEpochs) {
    ASSERT_TRUE(conn->query("CALL PROJECT_GRAPH('CsrG', ['CsrNode'], ['CsrEdge'])")->isSuccess());
    const auto& entry = getNativeEntry("CsrG");
    ASSERT_EQ(entry.relCsrEpochs.size(), 1u);
    const auto epochAtProjection = entry.relCsrEpochs[0];
    // Mutating the rel table bumps its change epoch; a fresh projection must record a later one,
    // which is what lets consumers detect that an old entry's pinned CSR is stale.
    ASSERT_TRUE(conn->query("MATCH (a:CsrNode {id:2}), (b:CsrNode {id:0}) "
                            "CREATE (a)-[:CsrEdge]->(b)")
                    ->isSuccess());
    ASSERT_TRUE(conn->query("CALL PROJECT_GRAPH('CsrG2', ['CsrNode'], ['CsrEdge'])")->isSuccess());
    const auto& entry2 = getNativeEntry("CsrG2");
    ASSERT_EQ(entry2.relCsrEpochs.size(), 1u);
    ASSERT_GT(entry2.relCsrEpochs[0], epochAtProjection);
}

TEST_F(ProjectGraphCsrTest, skipsMaterializationWithPredicate) {
    ASSERT_TRUE(
        conn->query("CALL PROJECT_GRAPH('CsrGPred', ['CsrNode'], {CsrEdge: 'r.rowid >= 0'})")
            ->isSuccess());
    const auto& entry = getNativeEntry("CsrGPred");
    ASSERT_TRUE(entry.relCsrResults.empty());
}

TEST_F(ProjectGraphCsrTest, skipsMaterializationWithMultipleNodeTables) {
    ASSERT_TRUE(conn->query("CREATE NODE TABLE CsrNode2(id INT64 PRIMARY KEY)")->isSuccess());
    ASSERT_TRUE(conn->query("CALL PROJECT_GRAPH('CsrGMulti', ['CsrNode', 'CsrNode2'], ['CsrEdge'])")
                    ->isSuccess());
    const auto& entry = getNativeEntry("CsrGMulti");
    ASSERT_TRUE(entry.relCsrResults.empty());
}

TEST_F(ProjectGraphCsrTest, handlesEmptyRelTable) {
    ASSERT_TRUE(conn->query("CREATE REL TABLE CsrEdgeEmpty(FROM CsrNode TO CsrNode)")->isSuccess());
    ASSERT_TRUE(
        conn->query("CALL PROJECT_GRAPH('CsrGEmpty', ['CsrNode'], ['CsrEdgeEmpty'])")->isSuccess());
    const auto& entry = getNativeEntry("CsrGEmpty");
    ASSERT_EQ(entry.relCsrResults.size(), 1u);
    // Zero edges: either unmaterialized (consumers fall back to scan) or a valid all-empty CSR.
    if (entry.relCsrResults[0] != nullptr) {
        auto* arrowResult = dynamic_cast<ArrowQueryResult*>(entry.relCsrResults[0].get());
        ASSERT_NE(arrowResult, nullptr);
        ASSERT_EQ(arrowResult->getCSRMetadata().indices.size(), 0u);
    }
}

TEST_F(ProjectGraphCsrTest, skipsMaterializationInManualTransaction) {
    ASSERT_TRUE(conn->query("BEGIN TRANSACTION")->isSuccess());
    ASSERT_TRUE(
        conn->query("CALL PROJECT_GRAPH('CsrGTxn', ['CsrNode'], ['CsrEdge'])")->isSuccess());
    ASSERT_TRUE(conn->query("COMMIT")->isSuccess());
    const auto& entry = getNativeEntry("CsrGTxn");
    ASSERT_TRUE(entry.relCsrResults.empty());
}

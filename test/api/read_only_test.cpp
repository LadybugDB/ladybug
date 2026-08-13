#include "api_test/api_test.h"
#include "graph/graph_entry_set.h"
#include "graph/parsed_graph_entry.h"

using namespace lbug::common;
using namespace lbug::graph;
using namespace lbug::testing;

class ReadOnlyTest : public ApiTest {};

TEST_F(ReadOnlyTest, Test) {
    if (databasePath == "" || databasePath == ":memory:") {
        return;
    }
    ASSERT_TRUE(
        conn->query("CREATE NODE TABLE Test (id INT64 PRIMARY KEY, arr INT64[4])")->isSuccess());
    systemConfig->readOnly = true;
    createDBAndConn();
    ASSERT_STREQ("Connection exception: Cannot execute write operations in a read-only database!",
        conn->query("CALL CLEAR_WARNINGS()")->toString().c_str());
    ASSERT_STREQ("Connection exception: Cannot execute write operations in a read-only database!",
        conn->query("CALL _CACHE_ARRAY_COLUMN_LOCALLY('Test', 'arr')")->toString().c_str());
}

TEST_F(ReadOnlyTest, ProjectGraphOnReadOnlyDatabase) {
    if (databasePath == "" || databasePath == ":memory:") {
        return;
    }
    ASSERT_TRUE(conn->query("CREATE NODE TABLE RoNode(id INT64 PRIMARY KEY)")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE REL TABLE RoEdge(FROM RoNode TO RoNode)")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:RoNode {id:0}), (:RoNode {id:1})")->isSuccess());
    ASSERT_TRUE(
        conn->query("MATCH (a:RoNode {id:0}), (b:RoNode {id:1}) "
                    "CREATE (a)-[:RoEdge]->(b)")
            ->isSuccess());
    systemConfig->readOnly = true;
    createDBAndConn();
    // The graph-projection trio only mutates the session-local GraphEntrySet — it must work on
    // a read-only database (project + run GDS over a read-only replica).
    ASSERT_TRUE(conn->query("CALL PROJECT_GRAPH('RoG', ['RoNode'], ['RoEdge'])")->isSuccess());
    // Eager arrow-CSR materialization runs an internal read query; it must also succeed here.
    auto* entrySet = GraphEntrySet::Get(*conn->getClientContext());
    ASSERT_TRUE(entrySet->hasGraph("RoG"));
    const auto& entry = entrySet->getEntry("RoG")->cast<ParsedNativeGraphEntry>();
    ASSERT_EQ(entry.relCsrResults.size(), 1u);
    ASSERT_NE(entry.relCsrResults[0], nullptr);
    ASSERT_TRUE(conn->query("CALL DROP_PROJECTED_GRAPH('RoG')")->isSuccess());
    ASSERT_FALSE(entrySet->hasGraph("RoG"));
}

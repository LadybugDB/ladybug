#include "graph_test/private_graph_test.h"
#include "main/connection.h"
#include "main/database.h"

namespace lbug {
namespace testing {

// Regression tests for MERGE with ON MATCH SET driven by a batch containing duplicate
// pattern keys. See https://github.com/LadybugDB/ladybug/pull/895.
class MergeOnMatchSetTest : public EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
        setUpTables();
    }

    void setUpTables() {
        ASSERT_TRUE(conn->query("CREATE NODE TABLE eventaccess (id STRING, PRIMARY KEY(id));")
                        ->isSuccess());
        ASSERT_TRUE(conn->query("CREATE NODE TABLE authentication (id STRING, PRIMARY KEY(id));")
                        ->isSuccess());
        ASSERT_TRUE(conn->query("CREATE REL TABLE access_test_property (FROM eventaccess TO "
                                "authentication, update_time TIMESTAMP, tag_code STRING, "
                                "MANY_MANY);")
                        ->isSuccess());
        ASSERT_TRUE(conn->query("CREATE (:eventaccess {id: 'e1'});")->isSuccess());
        ASSERT_TRUE(conn->query("CREATE (:eventaccess {id: 'e2'});")->isSuccess());
        ASSERT_TRUE(conn->query("CREATE (:eventaccess {id: 'event-1'});")->isSuccess());
        ASSERT_TRUE(conn->query("CREATE (:authentication {id: 'a1'});")->isSuccess());
        ASSERT_TRUE(conn->query("CREATE (:authentication {id: 'auth-1'});")->isSuccess());
    }

    // Runs the given query for each row and asserts success.
    void runQuery(const std::string& query) {
        auto result = conn->query(query);
        ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    }
};

// A cached parameterized MERGE followed by relationship SET must set each property on the
// correct relationship.
TEST_F(MergeOnMatchSetTest, CachedMergeSet) {
    static constexpr std::string_view query =
        "UNWIND [{eventaccess_id: '$ID', authentication_id: 'a1', update_time: "
        "'2026-01-01T00:00:00', tag_code: '$ID'}] AS row "
        "MATCH (e:eventaccess {id: row.eventaccess_id}) "
        "MATCH (a:authentication {id: row.authentication_id}) "
        "MERGE (e)-[r:access_test_property]->(a) "
        "SET r.update_time = TIMESTAMP(row.update_time), r.tag_code = row.tag_code;";
    for (auto id : {"e1", "e2"}) {
        auto idStr = std::string{id};
        auto rowQuery = std::string{query};
        for (auto pos = rowQuery.find("$ID"); pos != std::string::npos;
             pos = rowQuery.find("$ID")) {
            rowQuery.replace(pos, 3, idStr);
        }
        runQuery(rowQuery);
    }
    auto result = conn->query("MATCH (e:eventaccess)-[r:access_test_property]->(a:authentication) "
                              "RETURN e.id, r.tag_code ORDER BY e.id;");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNumTuples(), 2);
    auto tuple = result->getNext();
    ASSERT_EQ(tuple->getValue(0)->getValue<std::string>(), "e1");
    ASSERT_EQ(tuple->getValue(1)->getValue<std::string>(), "e1");
    tuple = result->getNext();
    ASSERT_EQ(tuple->getValue(0)->getValue<std::string>(), "e2");
    ASSERT_EQ(tuple->getValue(1)->getValue<std::string>(), "e2");
    ASSERT_FALSE(result->hasNext());
}

// A MERGE batch containing two rows with the same (source, target) pattern must apply the ON
// CREATE SET of the first row and the ON MATCH SET of the second row to the single merged
// relationship. Regression test: this used to read an uninitialized rel ID for the second ON
// MATCH SET executor and crash (or corrupt an unrelated relationship).
TEST_F(MergeOnMatchSetTest, DuplicateRows) {
    auto result = conn->query(
        "UNWIND ["
        "{source: 'event-1', target: 'auth-1', update_time: '2026-09-01 10:00:00', tag_code: "
        "'tag-1'},"
        "{source: 'event-1', target: 'auth-1', update_time: '2026-09-01 11:00:00', tag_code: "
        "'tag-2'}"
        "] AS row "
        "MATCH (s:eventaccess {id: row.source}) "
        "MATCH (t:authentication {id: row.target}) "
        "MERGE (s)-[r:access_test_property]->(t) "
        "ON CREATE SET r.update_time = TIMESTAMP(row.update_time), r.tag_code = row.tag_code "
        "ON MATCH SET r.update_time = TIMESTAMP(row.update_time), r.tag_code = row.tag_code "
        "WITH count(r) AS applied_count "
        "RETURN applied_count;");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    // Both input rows flow through the MERGE.
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);
    ASSERT_FALSE(result->hasNext());

    // Exactly one relationship exists and the second row's ON MATCH SET won.
    auto verify = conn->query(
        "MATCH (e:eventaccess {id: 'event-1'})-[r:access_test_property]->"
        "(a:authentication {id: 'auth-1'}) RETURN r.tag_code, CAST(r.update_time AS STRING);");
    ASSERT_TRUE(verify->isSuccess()) << verify->getErrorMessage();
    ASSERT_EQ(verify->getNumTuples(), 1);
    auto tuple = verify->getNext();
    ASSERT_EQ(tuple->getValue(0)->getValue<std::string>(), "tag-2");
    ASSERT_EQ(tuple->getValue(1)->getValue<std::string>(), "2026-09-01 11:00:00");
    ASSERT_FALSE(verify->hasNext());
}

// Post-write node and relationship deletes must be persisted to disk.
TEST_F(MergeOnMatchSetTest, PersistentDelete) {
    runQuery("CREATE NODE TABLE probe_person (id STRING, PRIMARY KEY(id));");
    runQuery("CREATE NODE TABLE probe_department (id STRING, PRIMARY KEY(id));");
    runQuery("CREATE REL TABLE probe_works_in (FROM probe_person TO probe_department, "
             "role STRING, MANY_MANY);");
    runQuery("UNWIND ['p1', 'p2', 'p3'] AS id MERGE (:probe_person {id: id});");
    runQuery("MERGE (:probe_department {id: 'd1'});");
    runQuery("UNWIND ["
             "{source: 'p1', target: 'd1', role: 'maintainer'},"
             "{source: 'p2', target: 'd1', role: 'member'}"
             "] AS row "
             "MATCH (p:probe_person {id: row.source}) "
             "MATCH (d:probe_department {id: row.target}) "
             "MERGE (p)-[r:probe_works_in]->(d) "
             "SET r.role = row.role;");
    runQuery("UNWIND [{id: 'p3'}] AS row WITH row WHERE row.id IS NOT NULL "
             "MATCH (e:probe_person {id: row.id}) DETACH DELETE e;");
    runQuery("UNWIND [{source: 'p2', target: 'd1'}] AS row "
             "MATCH (s:probe_person {id: row.source}) "
             "MATCH (s)-[r:probe_works_in]->(t:probe_department {id: row.target}) DELETE r;");

    // Reopen the same file read-only: successful reads prove that the mutations were
    // committed to disk rather than only kept in memory.
    conn.reset();
    database.reset();
    auto readOnlyConfig = *systemConfig;
    readOnlyConfig.readOnly = true;
    database = std::make_unique<main::Database>(databasePath, readOnlyConfig);
    conn = std::make_unique<main::Connection>(database.get());

    auto result = conn->query("MATCH (p:probe_person) RETURN count(p);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);
    ASSERT_FALSE(result->hasNext());

    auto relResult = conn->query(
        "MATCH (:probe_person {id: 'p2'})-[:probe_works_in]->(:probe_department {id: 'd1'}) "
        "RETURN count(*);");
    ASSERT_TRUE(relResult->isSuccess()) << relResult->getErrorMessage();
    ASSERT_EQ(relResult->getNext()->getValue(0)->getValue<int64_t>(), 0);
    ASSERT_FALSE(relResult->hasNext());
}

} // namespace testing
} // namespace lbug

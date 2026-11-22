#include <filesystem>
#include <fstream>
#include <iterator>

#include "common/file_system/local_file_system.h"
#include "graph_test/base_graph_test.h"
#include "graph_test/private_graph_test.h"

namespace lbug {
namespace testing {

static std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static void writeFile(const std::string& path, const std::string& content) {
    // Binary mode: in text mode Windows translates each '\n' into "\r\n", so writing the
    // CRLF fixture below would land "\r\r\n" in the file instead of the "\r\n" it means to
    // test.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

class ImportExportDBTest : public DBTest {
public:
    std::string getInputDir() override {
        return TestHelper::appendLbugRootPath("dataset/tinysnb/");
    }
};

TEST_F(ImportExportDBTest, Test) {
    auto tempDir = TestHelper::getTempDir(getTestGroupAndName());
    auto dbPath = common::LocalFileSystem::joinPath(tempDir, "export.tmp");
    // The tinysnb fixture contains INT128 columns and microsecond-precision INTERVAL values,
    // which EXPORT DATABASE (parquet) loudly rejects (#1176). Drop them so the export/import
    // round-trip exercises the success path; the knows-edge count assertion below is unaffected.
    for (const auto& query : std::vector<std::string>{"DROP TABLE movies", "DROP TABLE studyAt",
             "ALTER TABLE person DROP lastJobDuration", "ALTER TABLE organisation DROP history",
             "ALTER TABLE organisation DROP licenseValidInterval",
             "ALTER TABLE knows DROP validInterval"}) {
        auto dropResult = conn->query(query);
        ASSERT_TRUE(dropResult->isSuccess()) << dropResult->getErrorMessage();
    }
    auto result = conn->query("EXPORT DATABASE '" + dbPath + "'");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result.reset();
    createNewDB();
    result = conn->query("IMPORT DATABASE '" + dbPath + "'");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("MATCH (a:person)-[e:knows]->(b:person) RETURN COUNT(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 14);
}

class ImportExportSerialTest : public EmptyDBTest {
public:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
        std::filesystem::create_directories(TestHelper::getTempDir(getTestGroupAndName()));
    }
};

static void checkSerialCounters(main::Connection& conn) {
    // `used` was inserted into twice, so the next id is 2; `unused` was never inserted into, so
    // the next id is 0.
    auto result = conn.query("CREATE (:used); CREATE (:unused);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn.query("MATCH (u:used) WHERE u.id > 1 RETURN u.id");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 2);
    result = conn.query("MATCH (u:unused) RETURN u.id");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 0);
}

TEST_F(ImportExportSerialTest, ExportRestoresSerialWithSetval) {
    auto dir = common::LocalFileSystem::joinPath(TestHelper::getTempDir(getTestGroupAndName()),
        "export_serial");
    for (const auto& query :
        std::vector<std::string>{"CREATE NODE TABLE used(id SERIAL, PRIMARY KEY(id))",
            "CREATE NODE TABLE unused(id SERIAL, PRIMARY KEY(id))", "CREATE (:used)",
            "CREATE (:used)", "EXPORT DATABASE '" + dir + "'"}) {
        auto result = conn->query(query);
        ASSERT_TRUE(result->isSuccess()) << query << ": " << result->getErrorMessage();
    }
    auto schema = readFile(common::LocalFileSystem::joinPath(dir, "schema.cypher"));
    EXPECT_NE(schema.find("RETURN setval('used_id_serial', 1);"), std::string::npos) << schema;
    EXPECT_EQ(schema.find("DROP SEQUENCE"), std::string::npos) << schema;
    EXPECT_EQ(schema.find("unused_id_serial"), std::string::npos) << schema;
    main::Database db(common::LocalFileSystem::joinPath(dir, "imported.lbdb"));
    main::Connection importConn(&db);
    auto result = importConn.query("IMPORT DATABASE '" + dir + "'");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    checkSerialCounters(importConn);
}

TEST_F(ImportExportSerialTest, ImportMapsLegacyDropCreatePairOntoSetval) {
    auto dir = common::LocalFileSystem::joinPath(TestHelper::getTempDir(getTestGroupAndName()),
        "export_serial_legacy");
    for (const auto& query :
        std::vector<std::string>{"CREATE NODE TABLE used(id SERIAL, PRIMARY KEY(id))",
            "CREATE NODE TABLE unused(id SERIAL, PRIMARY KEY(id))", "CREATE (:used)",
            "CREATE (:used)", "EXPORT DATABASE '" + dir + "'"}) {
        auto result = conn->query(query);
        ASSERT_TRUE(result->isSuccess()) << query << ": " << result->getErrorMessage();
    }
    // Rewrite the setval into the drop/create pair that older versions exported.
    auto path = common::LocalFileSystem::joinPath(dir, "schema.cypher");
    auto schema = readFile(path);
    const std::string setval = "RETURN setval('used_id_serial', 1);";
    auto pos = schema.find(setval);
    ASSERT_NE(pos, std::string::npos) << schema;
    schema.replace(pos, setval.size(),
        "DROP SEQUENCE IF EXISTS `used_id_serial`;\n"
        "CREATE SEQUENCE IF NOT EXISTS `used_id_serial` START 1 INCREMENT 1 MINVALUE 0 "
        "MAXVALUE 9223372036854775807 NO CYCLE;\n"
        "RETURN nextval('used_id_serial');");
    writeFile(path, schema);
    main::Database db(common::LocalFileSystem::joinPath(dir, "imported.lbdb"));
    main::Connection importConn(&db);
    auto result = importConn.query("IMPORT DATABASE '" + dir + "'");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    checkSerialCounters(importConn);
    // The sequence still belongs to the table, so the table stays droppable. Without the rewrite
    // this test only fails at this DROP TABLE after a crash before the next checkpoint; the
    // refusal test for DROP SEQUENCE on a SERIAL sequence belongs with part 3.
    result = importConn.query("DROP TABLE used");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
}

// Runs `queries` in the fixture's database and exports it to `<tempDir>/<name>`.
static std::string exportWith(main::Connection& conn, const std::string& name,
    const std::string& tempDir, const std::vector<std::string>& queries) {
    auto dir = common::LocalFileSystem::joinPath(tempDir, name);
    for (const auto& query : queries) {
        auto result = conn.query(query);
        EXPECT_TRUE(result->isSuccess()) << query << ": " << result->getErrorMessage();
    }
    auto result = conn.query("EXPORT DATABASE '" + dir + "'");
    EXPECT_TRUE(result->isSuccess()) << result->getErrorMessage();
    return dir;
}

static int64_t queryInt(main::Connection& conn, const std::string& query) {
    auto result = conn.query(query);
    EXPECT_TRUE(result->isSuccess()) << query << ": " << result->getErrorMessage();
    return result->getNext()->getValue(0)->getValue<int64_t>();
}

TEST_F(ImportExportSerialTest, ExportKeepsPlainSequenceAsDropCreateNextval) {
    auto tempDir = TestHelper::getTempDir(getTestGroupAndName());
    auto dir = exportWith(*conn, "plain_sequence", tempDir,
        {"CREATE SEQUENCE s START 5 INCREMENT 2", "RETURN nextval('s')", "RETURN nextval('s')"});
    auto schema = readFile(common::LocalFileSystem::joinPath(dir, "schema.cypher"));
    EXPECT_NE(schema.find("DROP SEQUENCE IF EXISTS `s`;\nCREATE SEQUENCE IF NOT EXISTS `s` START "
                          "7 INCREMENT 2"),
        std::string::npos)
        << schema;
    EXPECT_NE(schema.find("RETURN nextval('s');"), std::string::npos) << schema;
    EXPECT_EQ(schema.find("setval"), std::string::npos) << schema;
    main::Database db(common::LocalFileSystem::joinPath(dir, "imported.lbdb"));
    main::Connection importConn(&db);
    auto result = importConn.query("IMPORT DATABASE '" + dir + "'");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(queryInt(importConn, "RETURN nextval('s')"), 9);
}

TEST_F(ImportExportSerialTest, SerialColumnOfRelTableRoundTrips) {
    auto tempDir = TestHelper::getTempDir(getTestGroupAndName());
    auto dir = exportWith(*conn, "serial_rel", tempDir,
        {"CREATE NODE TABLE n(id INT64, PRIMARY KEY(id))",
            "CREATE REL TABLE r(FROM n TO n, s SERIAL)", "CREATE (:n {id: 1})",
            "CREATE (:n {id: 2})", "MATCH (a:n {id: 1}), (b:n {id: 2}) CREATE (a)-[:r]->(b)",
            "MATCH (a:n {id: 2}), (b:n {id: 1}) CREATE (a)-[:r]->(b)"});
    auto schema = readFile(common::LocalFileSystem::joinPath(dir, "schema.cypher"));
    EXPECT_NE(schema.find("RETURN setval('r_s_serial', 1);"), std::string::npos) << schema;
    EXPECT_EQ(schema.find("DROP SEQUENCE"), std::string::npos) << schema;
    main::Database db(common::LocalFileSystem::joinPath(dir, "imported.lbdb"));
    main::Connection importConn(&db);
    auto result = importConn.query("IMPORT DATABASE '" + dir + "'");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    // The SERIAL values in the data file are read back, not regenerated, and the counter is
    // restored after them.
    ASSERT_EQ(queryInt(importConn, "MATCH (:n {id: 1})-[e:r]->(:n {id: 2}) RETURN e.s"), 0);
    ASSERT_EQ(queryInt(importConn, "MATCH (:n {id: 2})-[e:r]->(:n {id: 1}) RETURN e.s"), 1);
    ASSERT_EQ(queryInt(importConn, "RETURN currval('r_s_serial')"), 1);
}

TEST_F(ImportExportSerialTest, ImportWithIcebugDiskStorageFormatRunsSetval) {
    auto tempDir = TestHelper::getTempDir(getTestGroupAndName());
    auto dir = exportWith(*conn, "serial_icebug", tempDir,
        {"CREATE NODE TABLE used(id SERIAL, PRIMARY KEY(id))", "CREATE (:used)", "CREATE (:used)"});
    main::Database db(common::LocalFileSystem::joinPath(dir, "imported.lbdb"));
    main::Connection importConn(&db);
    auto result = importConn.query("IMPORT DATABASE '" + dir + "' (storage_format='icebug-disk')");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(queryInt(importConn, "RETURN currval('used_id_serial')"), 1);
}

TEST_F(ImportExportSerialTest, QuoteInTableNameSurvivesExportAndImport) {
    auto tempDir = TestHelper::getTempDir(getTestGroupAndName());
    auto dir = exportWith(*conn, "quoted_name", tempDir,
        {"CREATE NODE TABLE `o'b`(id SERIAL, PRIMARY KEY(id))", "CREATE (:`o'b`)",
            "CREATE SEQUENCE `it's` START 3", "RETURN nextval('it\\'s')"});
    auto schema = readFile(common::LocalFileSystem::joinPath(dir, "schema.cypher"));
    EXPECT_NE(schema.find("RETURN setval('o\\'b_id_serial', 0);"), std::string::npos) << schema;
    EXPECT_NE(schema.find("RETURN nextval('it\\'s');"), std::string::npos) << schema;
    main::Database db(common::LocalFileSystem::joinPath(dir, "imported.lbdb"));
    main::Connection importConn(&db);
    auto result = importConn.query("IMPORT DATABASE '" + dir + "'");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(queryInt(importConn, "RETURN currval('o\\'b_id_serial')"), 0);
    ASSERT_EQ(queryInt(importConn, "RETURN nextval('it\\'s')"), 4);
}

TEST_F(ImportExportSerialTest, LegacyDropCreatePairWithCrlfLineEndingsIsMapped) {
    auto tempDir = TestHelper::getTempDir(getTestGroupAndName());
    auto dir = exportWith(*conn, "serial_legacy_crlf", tempDir,
        {"CREATE NODE TABLE used(id SERIAL, PRIMARY KEY(id))", "CREATE (:used)", "CREATE (:used)"});
    auto path = common::LocalFileSystem::joinPath(dir, "schema.cypher");
    auto schema = readFile(path);
    const std::string setval = "RETURN setval('used_id_serial', 1);";
    auto pos = schema.find(setval);
    ASSERT_NE(pos, std::string::npos) << schema;
    schema.replace(pos, setval.size(),
        "DROP SEQUENCE IF EXISTS `used_id_serial`;\n"
        "CREATE SEQUENCE IF NOT EXISTS `used_id_serial` START 1 INCREMENT 1 MINVALUE 0 "
        "MAXVALUE 9223372036854775807 NO CYCLE;\n"
        "RETURN nextval('used_id_serial');");
    std::string crlf;
    for (const char c : schema) {
        if (c == '\n') {
            crlf += '\r';
        }
        crlf += c;
    }
    writeFile(path, crlf);
    main::Database db(common::LocalFileSystem::joinPath(dir, "imported.lbdb"));
    main::Connection importConn(&db);
    auto result = importConn.query("IMPORT DATABASE '" + dir + "'");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    // Mapped onto setval, so the sequence is still owned by the table and holds its counter.
    ASSERT_EQ(queryInt(importConn, "RETURN currval('used_id_serial')"), 1);
}

} // namespace testing
} // namespace lbug

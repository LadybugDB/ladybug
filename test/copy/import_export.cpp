#include "common/file_system/local_file_system.h"
#include "graph_test/base_graph_test.h"
#include "graph_test/private_graph_test.h"

namespace lbug {
namespace testing {

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

} // namespace testing
} // namespace lbug

#include "api_test/api_test.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "storage/index/hash_index.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "test_helper/test_helper.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::catalog;
using namespace lbug::storage;
using namespace lbug::testing;
using namespace lbug::main;
using namespace lbug::transaction;

namespace {

// The default built-in PK hash index is named PrimaryKeyIndex::DEFAULT_NAME ("_PK").
constexpr const char* PK_INDEX_NAME = PrimaryKeyIndex::DEFAULT_NAME;

class DropIndexTest : public ApiTest {
public:
    void SetUp() override {
        BaseGraphTest::SetUp();
        createDBAndConn();
    }

    NodeTable& getNodeTable(const std::string& tableName) {
        auto* entry = database->getCatalog()
                          ->getTableCatalogEntry(&DUMMY_CHECKPOINT_TRANSACTION, tableName)
                          ->ptrCast<NodeTableCatalogEntry>();
        return database->getStorageManager()->getTable(entry->getTableID())->cast<NodeTable>();
    }

    static void assertQuery(QueryResult& result) {
        ASSERT_TRUE(result.isSuccess()) << result.toString();
    }
};

} // namespace

// Drop the default hash PK index after inserting data; the index should disappear from storage
// while rows remain queryable through the PK-scan fallback.
TEST_F(DropIndexTest, DropDefaultHashIndex) {
    auto& con = *conn;
    assertQuery(*con.query("CREATE NODE TABLE diperson(ID INT64, name STRING, PRIMARY KEY(ID))"));
    const auto& pkIndexName = PK_INDEX_NAME;
    // The default hash PK index should be present.
    ASSERT_TRUE(getNodeTable("diperson").tryGetPKIndex() != nullptr);

    assertQuery(*con.query("CREATE (:diperson {ID: 1, name: 'Alice'})"));
    assertQuery(*con.query("CREATE (:diperson {ID: 2, name: 'Bob'})"));
    assertQuery(*con.query("CREATE (:diperson {ID: 3, name: 'Carol'})"));

    auto dropResult = con.query(std::format("DROP INDEX diperson.{}", pkIndexName));
    assertQuery(*dropResult);
    EXPECT_EQ(TestHelper::convertResultToString(*dropResult),
        std::vector<std::string>{std::format("Index {} has been dropped.", pkIndexName)});

    // Index is gone from both storage and catalog.
    EXPECT_EQ(getNodeTable("diperson").tryGetPKIndex(), nullptr);
    auto* catalog = database->getCatalog();
    auto* entry = catalog->getTableCatalogEntry(&DUMMY_CHECKPOINT_TRANSACTION, "diperson")
                      ->ptrCast<NodeTableCatalogEntry>();
    EXPECT_FALSE(
        catalog->containsIndex(&DUMMY_CHECKPOINT_TRANSACTION, entry->getTableID(), pkIndexName));

    // Rows are still queryable via the PK-scan fallback (no index).
    auto res = con.query("MATCH (p:diperson) WHERE p.ID = 2 RETURN p.name");
    assertQuery(*res);
    EXPECT_EQ(TestHelper::convertResultToString(*res), std::vector<std::string>{"Bob"});
    auto countRes = con.query("MATCH (p:diperson) RETURN COUNT(*)");
    assertQuery(*countRes);
    EXPECT_EQ(TestHelper::convertResultToString(*countRes), std::vector<std::string>{"3"});
}

// Dropping a non-existent index without IF EXISTS must error.
TEST_F(DropIndexTest, DropNonExistentIndexThrows) {
    auto& con = *conn;
    assertQuery(*con.query("CREATE NODE TABLE diperson2(ID INT64, name STRING, PRIMARY KEY(ID))"));
    auto result = con.query("DROP INDEX diperson2.nonexistent_index");
    EXPECT_FALSE(result->isSuccess());
}

// DROP INDEX IF EXISTS is idempotent and never errors, even when the table or index is missing.
TEST_F(DropIndexTest, DropIndexIfExists) {
    auto& con = *conn;
    assertQuery(*con.query("CREATE NODE TABLE diperson3(ID INT64, name STRING, PRIMARY KEY(ID))"));
    const auto& pkIndexName = PK_INDEX_NAME;
    ASSERT_TRUE(getNodeTable("diperson3").tryGetPKIndex() != nullptr);

    // Idempotent on a missing index.
    auto result = con.query("DROP INDEX IF EXISTS diperson3.nonexistent_index");
    assertQuery(*result);

    // Idempotent on a missing table.
    auto result2 = con.query("DROP INDEX IF EXISTS missingtable.someindex");
    assertQuery(*result2);

    // Actually drop the PK index.
    auto dropResult = con.query(std::format("DROP INDEX IF EXISTS diperson3.{}", pkIndexName));
    assertQuery(*dropResult);
    EXPECT_EQ(getNodeTable("diperson3").tryGetPKIndex(), nullptr);

    // Dropping again (already dropped) is a no-op.
    auto dropAgain = con.query(std::format("DROP INDEX IF EXISTS diperson3.{}", pkIndexName));
    assertQuery(*dropAgain);
}

// A dropped index stays dropped across a persist + reopen (WAL replay path).
TEST_F(DropIndexTest, DropIndexPersistsAcrossReopen) {
    if (databasePath == "" || databasePath == ":memory:") {
        GTEST_SKIP() << "Persist test requires an on-disk database.";
    }
    auto& con = *conn;
    assertQuery(*con.query("CREATE NODE TABLE diperson4(ID INT64, name STRING, PRIMARY KEY(ID))"));
    const auto& pkIndexName = PK_INDEX_NAME;
    assertQuery(*con.query("CREATE (:diperson4 {ID: 1, name: 'Alice'})"));
    assertQuery(*con.query("CREATE (:diperson4 {ID: 2, name: 'Bob'})"));

    assertQuery(*con.query(std::format("DROP INDEX diperson4.{}", pkIndexName)));
    EXPECT_EQ(getNodeTable("diperson4").tryGetPKIndex(), nullptr);

    // Force a checkpoint and reopen the database.
    assertQuery(*con.query("CHECKPOINT"));
    conn.reset();
    database.reset();

    database = std::make_unique<Database>(databasePath, *systemConfig);
    conn = std::make_unique<Connection>(database.get());

    EXPECT_EQ(getNodeTable("diperson4").tryGetPKIndex(), nullptr);
    auto res = conn->query("MATCH (p:diperson4) WHERE p.ID = 2 RETURN p.name");
    assertQuery(*res);
    EXPECT_EQ(TestHelper::convertResultToString(*res), std::vector<std::string>{"Bob"});
}

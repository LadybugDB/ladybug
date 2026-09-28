#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "graph_test/private_graph_test.h"
#include "storage/index/hash_index.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "test_helper/test_helper.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::catalog;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace testing {

// Regression tests for #464: checkpointing an empty STRING primary-key index must not persist an
// overflow header page without the index header pages, which the constructor rejects on reopen.
class PKIndexCheckpointTest : public EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }

    void assertQuery(const std::string& query) const {
        auto result = conn->query(query);
        ASSERT_TRUE(result->isSuccess()) << query << ": " << result->toString();
    }

    const PrimaryKeyIndexStorageInfo& getPKStorageInfo(const std::string& tableName) const {
        auto* entry = database->getCatalog()
                          ->getTableCatalogEntry(&DUMMY_CHECKPOINT_TRANSACTION, tableName)
                          ->ptrCast<NodeTableCatalogEntry>();
        auto& nodeTable =
            database->getStorageManager()->getTable(entry->getTableID())->cast<NodeTable>();
        return nodeTable.getPKIndex()->getStorageInfo().constCast<PrimaryKeyIndexStorageInfo>();
    }

    // Both header pointers are assigned together, so an unset index header page implies an unset
    // overflow header page.
    void assertHeaderPagesConsistent(const std::string& tableName) const {
        const auto& info = getPKStorageInfo(tableName);
        if (info.firstHeaderPage == INVALID_PAGE_IDX) {
            EXPECT_EQ(info.overflowHeaderPage, INVALID_PAGE_IDX)
                << "overflow header page persisted without index header pages";
        }
    }

    void reopen() {
        conn.reset();
        createDBAndConn();
    }
};

TEST_F(PKIndexCheckpointTest, EmptyStringPKCheckpointAfterAlter) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    assertQuery("CREATE NODE TABLE t(id STRING PRIMARY KEY);");
    assertQuery("ALTER TABLE t ADD v INT64;");
    assertQuery("CHECKPOINT;");
    assertHeaderPagesConsistent("t");

    ASSERT_NO_THROW(reopen());
    auto result = conn->query("MATCH (n:t) RETURN count(*);");
    ASSERT_TRUE(result->isSuccess()) << result->toString();
    EXPECT_EQ(TestHelper::convertResultToString(*result), std::vector<std::string>{"0"});
    assertHeaderPagesConsistent("t");
}

TEST_F(PKIndexCheckpointTest, InsertAfterEmptyStringPKCheckpoint) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    assertQuery("CREATE NODE TABLE t(id STRING PRIMARY KEY);");
    assertQuery("ALTER TABLE t ADD v INT64;");
    assertQuery("CHECKPOINT;");
    assertQuery("CREATE (:t {id: 'a-key-longer-than-twelve-bytes', v: 1});");
    assertQuery("CREATE (:t {id: 'short', v: 2});");
    assertQuery("CHECKPOINT;");
    assertHeaderPagesConsistent("t");

    ASSERT_NO_THROW(reopen());
    auto result = conn->query("MATCH (n:t) WHERE n.id = 'a-key-longer-than-twelve-bytes' "
                              "RETURN n.v;");
    ASSERT_TRUE(result->isSuccess()) << result->toString();
    EXPECT_EQ(TestHelper::convertResultToString(*result), std::vector<std::string>{"1"});
    result = conn->query("MATCH (n:t) WHERE n.id = 'short' RETURN n.v;");
    ASSERT_TRUE(result->isSuccess()) << result->toString();
    EXPECT_EQ(TestHelper::convertResultToString(*result), std::vector<std::string>{"2"});
    // The primary key must still reject duplicates after reopen.
    result = conn->query("CREATE (:t {id: 'a-key-longer-than-twelve-bytes', v: 3});");
    EXPECT_FALSE(result->isSuccess());
    const auto& info = getPKStorageInfo("t");
    EXPECT_NE(info.firstHeaderPage, INVALID_PAGE_IDX);
    EXPECT_NE(info.overflowHeaderPage, INVALID_PAGE_IDX);
}

} // namespace testing
} // namespace lbug

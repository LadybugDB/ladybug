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

// Regression test for #1510: one checkpoint can grow an index enough to split a slot created
// earlier in the same pass, whose overflow chain then still lives in the pass's buffer of new
// overflow slots.
TEST_F(PKIndexCheckpointTest, SplitOfChainInNewOverflowSlotsKeepsKeysReachable) {
    // The keys below are chosen for an index that starts at 16 slots, one 4 KiB page of them.
    if (inMemMode || LBUG_PAGE_SIZE != 4096) {
        GTEST_SKIP();
    }
    // Low hash bits 1100011 put the chained keys in slot 3 of a 16-slot sub-index, across a primary
    // and an overflow slot. Growing that sub-index to 100 slots moves them to slot 35, then
    // slot 99.
    constexpr uint64_t chainedBits = 0b1100011;
    constexpr uint64_t chainedMask = 0b1111111;
    constexpr size_t numChained = 18;
    constexpr size_t numOthers = 582;
    std::vector<std::string> chained;
    std::vector<std::string> others;
    for (auto i = 0u; chained.size() < numChained || others.size() < numOthers; i++) {
        auto key = "long-enough-primary-key-" + std::to_string(i);
        const auto hash = HashIndexUtils::hash(key);
        if (HashIndexUtils::getHashIndexPositionForHash(hash) != 0) {
            continue;
        }
        const auto isChained = (hash & chainedMask) == chainedBits;
        auto& keys = isChained ? chained : others;
        if (keys.size() < (isChained ? numChained : numOthers)) {
            keys.push_back(std::move(key));
        }
    }
    const auto createNodes = [&](const std::vector<std::string>& keys) {
        std::string list;
        for (const auto& key : keys) {
            list += "'" + key + "',";
        }
        list.pop_back();
        assertQuery("UNWIND [" + list + "] AS k CREATE (:t {id: k});");
    };

    assertQuery("CREATE NODE TABLE t(id STRING PRIMARY KEY);");
    createNodes(chained);
    assertQuery("CHECKPOINT;");
    createNodes(others);
    assertQuery("CHECKPOINT;");

    ASSERT_NO_THROW(reopen());
    for (const auto* keys : {&chained, &others}) {
        for (const auto& key : *keys) {
            auto result = conn->query("MATCH (n:t {id: '" + key + "'}) RETURN count(n);");
            ASSERT_TRUE(result->isSuccess()) << result->toString();
            EXPECT_EQ(TestHelper::convertResultToString(*result), std::vector<std::string>{"1"})
                << key;
        }
    }
}

} // namespace testing
} // namespace lbug

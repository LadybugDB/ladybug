#include <string>

#include "catalog/catalog.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/constants.h"
#include "graph_test/private_graph_test.h"
#include "main/connection.h"
#include "storage/storage_manager.h"
#include "storage/storage_utils.h"
#include "storage/table/node_table.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace testing {

class NodeVisibilityCacheTest : public EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }
};

// A COPY appends node groups before it commits, so a cache made meanwhile counts them. When the
// COPY rolls back, those groups are removed, and the cache must not read them.
TEST_F(NodeVisibilityCacheTest, GroupRemovedByRolledBackCopyIsNotVisible) {
    ASSERT_TRUE(conn->query("CREATE NODE TABLE N(id INT64 PRIMARY KEY);")->isSuccess());
    ASSERT_TRUE(conn->query("UNWIND range(0, 9) AS i CREATE (:N {id: i});")->isSuccess());
    ASSERT_TRUE(conn->query("BEGIN TRANSACTION;")->isSuccess());
    const auto lastId = std::to_string(2 * StorageConfig::NODE_GROUP_SIZE);
    auto copy = conn->query("COPY N FROM (UNWIND range(10, " + lastId + ") AS i RETURN i);");
    ASSERT_TRUE(copy->isSuccess()) << copy->getErrorMessage();

    auto reader = std::make_unique<main::Connection>(database.get());
    ASSERT_TRUE(reader->query("BEGIN TRANSACTION READ ONLY;")->isSuccess());
    auto* context = getClientContext(*reader);
    auto* transaction = Transaction::Get(*context);
    const auto tableID =
        catalog::Catalog::Get(*context)->getTableCatalogEntry(transaction, "N")->getTableID();
    auto& table = StorageManager::Get(*context)->getTable(tableID)->cast<NodeTable>();
    const auto numGroupsWithCopy = table.getNumNodeGroups();
    ASSERT_EQ(numGroupsWithCopy, 3u);
    NodeVisibilityCache cache{table, transaction};

    ASSERT_TRUE(conn->query("ROLLBACK;")->isSuccess());
    ASSERT_EQ(table.getNumNodeGroups(), 1u);
    EXPECT_TRUE(cache.isVisible(9));
    EXPECT_FALSE(cache.isVisible(10));
    EXPECT_FALSE(cache.isVisible(StorageUtils::getStartOffsetOfNodeGroup(1)));
    EXPECT_FALSE(cache.isVisible(StorageUtils::getStartOffsetOfNodeGroup(2)));
    ASSERT_TRUE(reader->query("COMMIT;")->isSuccess());
}

} // namespace testing
} // namespace lbug

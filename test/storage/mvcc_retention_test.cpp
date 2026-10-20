#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/serializer/buffer_reader.h"
#include "common/serializer/buffer_writer.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "graph_test/private_graph_test.h"
#include "main/connection.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/table/node_table.h"
#include "storage/table/update_info.h"
#include "storage/table/version_info.h"
#include "transaction/transaction.h"
#include "transaction/transaction_manager.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace testing {

class MVCCRetentionTest : public EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }

    void TearDown() override { EmptyDBTest::TearDown(); }

    TransactionManager* getTM() { return database->getTransactionManager(); }

    static void setIntValue(ValueVector& values, int64_t value) {
        values.setValue<int64_t>(0, value);
    }
};

TEST_F(MVCCRetentionTest, EmptyHorizonIsNone) {
    EXPECT_EQ(getTM()->snapshotHorizon(), INVALID_TRANSACTION);
}

TEST_F(MVCCRetentionTest, ReadTransactionPinsAndReleasesHorizon) {
    ASSERT_TRUE(conn->query("BEGIN TRANSACTION READ ONLY;")->isSuccess());
    const auto pinned = getTM()->snapshotHorizon();
    EXPECT_NE(pinned, INVALID_TRANSACTION);
    ASSERT_TRUE(conn->query("COMMIT;")->isSuccess());
    EXPECT_EQ(getTM()->snapshotHorizon(), INVALID_TRANSACTION);
}

TEST_F(MVCCRetentionTest, RollbackReleasesHorizon) {
    ASSERT_TRUE(conn->query("BEGIN TRANSACTION;")->isSuccess());
    EXPECT_NE(getTM()->snapshotHorizon(), INVALID_TRANSACTION);
    ASSERT_TRUE(conn->query("ROLLBACK;")->isSuccess());
    EXPECT_EQ(getTM()->snapshotHorizon(), INVALID_TRANSACTION);
}

TEST_F(MVCCRetentionTest, HorizonIsMinimumOverOpenTransactions) {
    auto readerB = std::make_unique<main::Connection>(database.get());
    ASSERT_TRUE(conn->query("BEGIN TRANSACTION READ ONLY;")->isSuccess());
    const auto first = getTM()->snapshotHorizon();
    // A write between the two begins separates their start timestamps.
    ASSERT_TRUE(readerB->query("CREATE NODE TABLE H(id INT64 PRIMARY KEY);")->isSuccess());
    ASSERT_TRUE(readerB->query("BEGIN TRANSACTION READ ONLY;")->isSuccess());
    const auto second = getTM()->snapshotHorizon();
    // The first reader opened earlier, so the horizon stays pinned at its start timestamp.
    EXPECT_EQ(second, first);
    EXPECT_NE(second, INVALID_TRANSACTION);
    ASSERT_TRUE(conn->query("COMMIT;")->isSuccess());
    const auto afterFirstClose = getTM()->snapshotHorizon();
    EXPECT_NE(afterFirstClose, INVALID_TRANSACTION);
    EXPECT_GE(afterFirstClose, first);
    ASSERT_TRUE(readerB->query("COMMIT;")->isSuccess());
    EXPECT_EQ(getTM()->snapshotHorizon(), INVALID_TRANSACTION);
    readerB.reset();
}

TEST_F(MVCCRetentionTest, OpenStartTSsTracksOpenTransactions) {
    EXPECT_TRUE(getTM()->snapshotOpenStartTSs().empty());
    ASSERT_TRUE(conn->query("BEGIN TRANSACTION READ ONLY;")->isSuccess());
    const auto open = getTM()->snapshotOpenStartTSs();
    ASSERT_EQ(open.size(), 1u);
    EXPECT_EQ(open[0], getTM()->snapshotHorizon());
    ASSERT_TRUE(conn->query("COMMIT;")->isSuccess());
    EXPECT_TRUE(getTM()->snapshotOpenStartTSs().empty());
}

TEST_F(MVCCRetentionTest, AutoTransactionDoesNotPinHorizon) {
    ASSERT_TRUE(conn->query("CREATE NODE TABLE H(id INT64 PRIMARY KEY);")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:H {id: 1});")->isSuccess());
    // Auto-commit queries register only for their own duration.
    EXPECT_EQ(getTM()->snapshotHorizon(), INVALID_TRANSACTION);
}

TEST(VersionRetentionTest, InsertCommitTimestampsRoundTrip) {
    VersionInfo info;
    constexpr auto writerID = Transaction::START_TRANSACTION_ID;
    info.append(writerID, 0, 3);
    info.commitInsert(0, 3, 10);
    EXPECT_EQ(info.getInsertCommitTS(0), 10u);
    EXPECT_EQ(info.getInsertCommitTS(2), 10u);
    EXPECT_EQ(info.getInsertCommitTS(2 * DEFAULT_VECTOR_CAPACITY), INVALID_TRANSACTION);
    EXPECT_FALSE(info.hasRetainedEntries(11));
    EXPECT_TRUE(info.hasRetainedEntries(10));
}

TEST(VersionRetentionTest, FreshStampsFilterAtHorizon) {
    VersionInfo info;
    info.stampInsertCommitTS(5, 12);
    EXPECT_EQ(info.getInsertCommitTS(5), 12u);
    // Fresh vectors default unstamped rows to baked-visible rather than absent.
    EXPECT_EQ(info.getInsertCommitTS(6), 0u);
    // Unstamped rows in a fresh vector stay baked-visible.
    const Transaction oldReader{TransactionType::READ_ONLY, 7, 11};
    EXPECT_TRUE(info.isInserted(&oldReader, 6));
    EXPECT_FALSE(info.isInserted(&oldReader, 5));
    const Transaction newReader{TransactionType::READ_ONLY, 8, 12};
    EXPECT_TRUE(info.isInserted(&newReader, 5));
    EXPECT_TRUE(info.hasRetainedEntries(12));
    EXPECT_FALSE(info.hasRetainedEntries(13));
}

TEST(VersionRetentionTest, DeleteStampsFilterAtHorizon) {
    VersionInfo info;
    info.delete_(Transaction::DUMMY_TRANSACTION_ID, 7);
    info.stampDeleteCommitTS(7, 20);
    EXPECT_EQ(info.getDeleteCommitTS(7), 20u);
    EXPECT_EQ(info.getDeleteCommitTS(8), INVALID_TRANSACTION);
    const Transaction oldReader{TransactionType::READ_ONLY, 9, 15};
    EXPECT_FALSE(info.isDeleted(&oldReader, 7));
    const Transaction newReader{TransactionType::READ_ONLY, 10, 25};
    EXPECT_TRUE(info.isDeleted(&newReader, 7));
    EXPECT_TRUE(info.hasRetainedEntries(20));
    EXPECT_FALSE(info.hasRetainedEntries(21));
}

TEST(VersionRetentionTest, DeletedRowsAcrossVectorBoundaries) {
    VersionInfo info;
    constexpr auto writerID = Transaction::START_TRANSACTION_ID;
    constexpr row_idx_t numRows = 2 * DEFAULT_VECTOR_CAPACITY + 10;
    info.append(writerID, 0, numRows);
    info.commitInsert(0, numRows, 5);
    const std::vector<row_idx_t> committed{DEFAULT_VECTOR_CAPACITY - 1, DEFAULT_VECTOR_CAPACITY,
        numRows - 1};
    for (const auto row : committed) {
        info.delete_(writerID + 1, row);
        info.commitDelete(row, 1, 6);
    }
    info.delete_(writerID + 2, 3); // Not committed: hidden from every other transaction.
    const Transaction reader{TransactionType::READ_ONLY, 7, 10};
    std::vector<row_idx_t> rows;
    info.getDeletedRows(&reader, 0, numRows, rows);
    EXPECT_EQ(rows, committed);
    EXPECT_EQ(info.getNumDeletions(&reader, 0, numRows), committed.size());
    rows.clear();
    info.getDeletedRows(&reader, DEFAULT_VECTOR_CAPACITY, DEFAULT_VECTOR_CAPACITY + 9, rows);
    EXPECT_EQ(rows, std::vector<row_idx_t>{DEFAULT_VECTOR_CAPACITY});
    rows.clear();
    const Transaction olderReader{TransactionType::READ_ONLY, 8, 5};
    info.getDeletedRows(&olderReader, 0, numRows, rows);
    EXPECT_TRUE(rows.empty());
}

TEST(VersionRetentionTest, InsertedRowsIgnoreDeletions) {
    VersionInfo info;
    constexpr auto writerID = Transaction::START_TRANSACTION_ID;
    constexpr row_idx_t numCommitted = DEFAULT_VECTOR_CAPACITY + 5;
    info.append(writerID, 0, numCommitted);
    info.commitInsert(0, numCommitted, 5);
    info.append(writerID + 1, numCommitted, 3); // Not committed.
    info.delete_(writerID + 2, 1);
    info.commitDelete(1, 1, 6);
    const Transaction reader{TransactionType::READ_ONLY, 7, 10};
    EXPECT_EQ(info.getNumInsertedRows(&reader, 0, numCommitted + 3), numCommitted);
    EXPECT_EQ(info.getNumVisibleRows(&reader, 0, numCommitted + 3), numCommitted - 1);
    const Transaction olderReader{TransactionType::READ_ONLY, 8, 4};
    EXPECT_EQ(info.getNumInsertedRows(&olderReader, 0, numCommitted + 3), 0u);
}

TEST(VersionRetentionTest, BakedEntriesDoNotCountAsRetained) {
    VersionInfo info;
    constexpr auto writerID = Transaction::START_TRANSACTION_ID;
    info.append(writerID, 0, 2);
    info.commitInsert(0, 2, 5);
    info.delete_(Transaction::DUMMY_TRANSACTION_ID, 0);
    info.commitDelete(0, 1, 6);
    // Everything committed below the horizon: retained set is empty.
    EXPECT_FALSE(info.hasRetainedEntries(10));
    // Uncommitted entries always count: no type test needed on the retain path.
    VersionInfo pending;
    pending.append(writerID + 42, 0, 1);
    EXPECT_TRUE(pending.hasRetainedEntries(10));
}

TEST(VersionRetentionTest, RetainedInsertsCollapseOnTheWire) {
    VersionInfo info;
    info.stampInsertCommitTS(5, 12);
    info.delete_(Transaction::DUMMY_TRANSACTION_ID, 9);
    info.stampDeleteCommitTS(9, 14);
    const auto writer = std::make_shared<BufferWriter>();
    Serializer ser{writer};
    info.serialize(ser);
    Deserializer deser{std::make_unique<BufferReader>(writer->getBlobData(), writer->getSize())};
    auto restored = VersionInfo::deserialize(deser);
    // Recovery collapses retained inserts to baked-visible: no pre-restart reader survives
    // and post-restart timestamps start fresh, so this resolves identically.
    const Transaction oldReader{TransactionType::READ_ONLY, 7, 11};
    EXPECT_TRUE(restored->isInserted(&oldReader, 5));
    // Retained deletes collapse to always-deleted on the wire: post-restart timestamps
    // start fresh (a reopened database never assigns the stored stamp), so the delete
    // must already hold for the oldest possible reader.
    EXPECT_TRUE(restored->isDeleted(&oldReader, 9));
    const Transaction newReader{TransactionType::READ_ONLY, 8, 14};
    EXPECT_TRUE(restored->isDeleted(&newReader, 9));
    // Post-restart readers observe both the collapsed insert and the collapsed delete.
    const Transaction restartedReader{TransactionType::READ_ONLY, 50, 1};
    EXPECT_TRUE(restored->isInserted(&restartedReader, 5));
    EXPECT_TRUE(restored->isDeleted(&restartedReader, 9));
}

TEST_F(MVCCRetentionTest, RetainedDeleteSurvivesReopen) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE NODE TABLE P(id INT64 PRIMARY KEY, v INT64);")->isSuccess());
    ASSERT_TRUE(conn->query("UNWIND range(1, 5) AS i CREATE (:P {id: i, v: 0});")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    auto reader = std::make_unique<main::Connection>(database.get());
    ASSERT_TRUE(reader->query("BEGIN TRANSACTION READ ONLY;")->isSuccess());
    ASSERT_TRUE(conn->query("MATCH (p:P {id: 3}) DETACH DELETE p;")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    ASSERT_TRUE(reader->query("COMMIT;")->isSuccess());
    reader.reset();
    // Reopen: the committed delete must stay deleted; the row must not come back.
    createDBAndConn();
    auto res = conn->query("MATCH (p:P) RETURN p.id ORDER BY p.id;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    std::vector<int64_t> ids;
    while (res->hasNext()) {
        ids.push_back(res->getNext()->getValue(0)->getValue<int64_t>());
    }
    EXPECT_EQ(ids, std::vector<int64_t>({1, 2, 4, 5}));
}

TEST_F(MVCCRetentionTest, RetainedDeleteSurvivesCrash) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CALL force_checkpoint_on_close=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE NODE TABLE P(id INT64 PRIMARY KEY, v INT64);")->isSuccess());
    ASSERT_TRUE(conn->query("UNWIND range(1, 5) AS i CREATE (:P {id: i, v: 0});")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    auto reader = std::make_unique<main::Connection>(database.get());
    ASSERT_TRUE(reader->query("BEGIN TRANSACTION READ ONLY;")->isSuccess());
    ASSERT_TRUE(conn->query("MATCH (p:P {id: 3}) DETACH DELETE p;")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    ASSERT_TRUE(reader->query("COMMIT;")->isSuccess());
    reader.reset();
    // Simulate a crash: drop the connections without a closing checkpoint.
    conn.reset();
    database.reset();
    createDBAndConn();
    auto res = conn->query("MATCH (p:P) RETURN p.id ORDER BY p.id;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    std::vector<int64_t> ids;
    while (res->hasNext()) {
        ids.push_back(res->getNext()->getValue(0)->getValue<int64_t>());
    }
    EXPECT_EQ(ids, std::vector<int64_t>({1, 2, 4, 5}));
}

TEST_F(MVCCRetentionTest, CommittedUpdatesSurvivePinnedCheckpoint) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE NODE TABLE P(id INT64 PRIMARY KEY, v INT64);")->isSuccess());
    ASSERT_TRUE(conn->query("UNWIND range(1, 5) AS i CREATE (:P {id: i, v: 0});")->isSuccess());
    // No checkpoint before the updates: the groups are still in memory.
    ASSERT_TRUE(conn->query("MATCH (p:P {id: 1}) SET p.v = 10;")->isSuccess());
    ASSERT_TRUE(conn->query("MATCH (p:P {id: 2}) SET p.v = 20;")->isSuccess());
    auto reader = std::make_unique<main::Connection>(database.get());
    ASSERT_TRUE(reader->query("BEGIN TRANSACTION READ ONLY;")->isSuccess());
    ASSERT_TRUE(conn->query("MATCH (p:P {id: 1}) SET p.v = 30;")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    // The parked reader still observes the pre-begin values.
    auto parked = reader->query("MATCH (p:P {id: 2}) RETURN p.v;");
    ASSERT_TRUE(parked->isSuccess()) << parked->getErrorMessage();
    ASSERT_TRUE(parked->hasNext());
    EXPECT_EQ(parked->getNext()->getValue(0)->getValue<int64_t>(), 20);
    parked = reader->query("MATCH (p:P {id: 1}) RETURN p.v;");
    ASSERT_TRUE(parked->isSuccess()) << parked->getErrorMessage();
    ASSERT_TRUE(parked->hasNext());
    EXPECT_EQ(parked->getNext()->getValue(0)->getValue<int64_t>(), 10);
    ASSERT_TRUE(reader->query("COMMIT;")->isSuccess());
    reader.reset();
    // The committed updates are durable, not just visible to the parked reader.
    auto fresh = conn->query("MATCH (p:P {id: 2}) RETURN p.v;");
    ASSERT_TRUE(fresh->isSuccess()) << fresh->getErrorMessage();
    ASSERT_TRUE(fresh->hasNext());
    EXPECT_EQ(fresh->getNext()->getValue(0)->getValue<int64_t>(), 20);
    fresh = conn->query("MATCH (p:P {id: 1}) RETURN p.v;");
    ASSERT_TRUE(fresh->isSuccess()) << fresh->getErrorMessage();
    ASSERT_TRUE(fresh->hasNext());
    EXPECT_EQ(fresh->getNext()->getValue(0)->getValue<int64_t>(), 30);
    // And they survive a reopen.
    createDBAndConn();
    auto reopened = conn->query("MATCH (p:P) RETURN p.id, p.v ORDER BY p.id;");
    ASSERT_TRUE(reopened->isSuccess()) << reopened->getErrorMessage();
    std::vector<std::pair<int64_t, int64_t>> rows;
    while (reopened->hasNext()) {
        auto row = reopened->getNext();
        rows.emplace_back(row->getValue(0)->getValue<int64_t>(),
            row->getValue(1)->getValue<int64_t>());
    }
    const std::vector<std::pair<int64_t, int64_t>> expectedRows = {{1, 30}, {2, 20}, {3, 0}, {4, 0},
        {5, 0}};
    EXPECT_EQ(rows, expectedRows);
}

TEST_F(MVCCRetentionTest, NoWriteCheckpointReclaimsRetention) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    ASSERT_TRUE(conn->query("CALL auto_checkpoint=false;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE NODE TABLE P(id INT64 PRIMARY KEY, v INT64);")->isSuccess());
    ASSERT_TRUE(conn->query("UNWIND range(1, 5) AS i CREATE (:P {id: i, v: 0});")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    auto tableHasRetainedHorizon = [&]() {
        auto* entry = database->getCatalog()
                          ->getTableCatalogEntry(&transaction::DUMMY_CHECKPOINT_TRANSACTION, "P")
                          ->ptrCast<catalog::NodeTableCatalogEntry>();
        return database->getStorageManager()
            ->getTable(entry->getTableID())
            ->cast<storage::NodeTable>()
            .hasRetainedHorizon();
    };
    EXPECT_FALSE(tableHasRetainedHorizon());
    auto reader = std::make_unique<main::Connection>(database.get());
    ASSERT_TRUE(reader->query("BEGIN TRANSACTION READ ONLY;")->isSuccess());
    ASSERT_TRUE(conn->query("MATCH (p:P {id: 2}) SET p.v = 20;")->isSuccess());
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    EXPECT_TRUE(tableHasRetainedHorizon());
    ASSERT_TRUE(reader->query("COMMIT;")->isSuccess());
    reader.reset();
    // No further writes: the next checkpoint must still reclaim the retained state.
    ASSERT_TRUE(conn->query("CHECKPOINT;")->isSuccess());
    EXPECT_FALSE(tableHasRetainedHorizon());
    auto res = conn->query("MATCH (p:P) RETURN p.id, p.v ORDER BY p.id;");
    ASSERT_TRUE(res->isSuccess()) << res->getErrorMessage();
    std::vector<std::pair<int64_t, int64_t>> rows;
    while (res->hasNext()) {
        auto row = res->getNext();
        rows.emplace_back(row->getValue(0)->getValue<int64_t>(),
            row->getValue(1)->getValue<int64_t>());
    }
    const std::vector<std::pair<int64_t, int64_t>> expectedRows = {{1, 0}, {2, 20}, {3, 0}, {4, 0},
        {5, 0}};
    EXPECT_EQ(rows, expectedRows);
}

TEST_F(MVCCRetentionTest, UpdateRetentionPlanClassifiesChains) {
    auto* mm = getMemoryManager(*database);
    UpdateInfo updateInfo;
    ValueVector values{LogicalType::INT64()};
    values.setState(std::make_shared<DataChunkState>(DEFAULT_VECTOR_CAPACITY));
    setIntValue(values, 100);
    Transaction firstWrite{*conn->getClientContext(), TransactionType::WRITE,
        Transaction::START_TRANSACTION_ID, 0};
    auto& firstNode = updateInfo.update(*mm, &firstWrite, 0, 3, values);
    updateInfo.commit(0, &firstNode, 5);
    setIntValue(values, 200);
    Transaction secondWrite{*conn->getClientContext(), TransactionType::WRITE,
        Transaction::START_TRANSACTION_ID + 1, 6};
    auto& secondNode = updateInfo.update(*mm, &secondWrite, 0, 3, values);
    updateInfo.commit(0, &secondNode, 12);

    const auto horizonPlan = updateInfo.planRetention(10);
    ASSERT_EQ(horizonPlan.size(), 1u);
    EXPECT_EQ(horizonPlan[0].kept.size(), 1u);
    EXPECT_EQ(horizonPlan[0].kept[0]->version, 12u);
    ASSERT_NE(horizonPlan[0].newestBelow, nullptr);
    EXPECT_EQ(horizonPlan[0].newestBelow->version, 5u);
    EXPECT_TRUE(horizonPlan[0].keptRows.test(3));
    EXPECT_TRUE(horizonPlan[0].belowRows.test(3));
    EXPECT_TRUE(UpdateInfo::plansHaveKeptNodes(horizonPlan));

    const auto pastPlan = updateInfo.planRetention(20);
    ASSERT_EQ(pastPlan.size(), 1u);
    EXPECT_TRUE(pastPlan[0].kept.empty());
    EXPECT_FALSE(UpdateInfo::plansHaveKeptNodes(pastPlan));
}

TEST_F(MVCCRetentionTest, DropKeepsNewestBelowHorizonNode) {
    auto* mm = getMemoryManager(*database);
    UpdateInfo updateInfo;
    ValueVector values{LogicalType::INT64()};
    values.setState(std::make_shared<DataChunkState>(DEFAULT_VECTOR_CAPACITY));
    setIntValue(values, 100);
    Transaction firstWrite{*conn->getClientContext(), TransactionType::WRITE,
        Transaction::START_TRANSACTION_ID, 0};
    auto& firstNode = updateInfo.update(*mm, &firstWrite, 0, 3, values);
    updateInfo.commit(0, &firstNode, 5);
    setIntValue(values, 200);
    Transaction secondWrite{*conn->getClientContext(), TransactionType::WRITE,
        Transaction::START_TRANSACTION_ID + 1, 6};
    auto& secondNode = updateInfo.update(*mm, &secondWrite, 0, 3, values);
    updateInfo.commit(0, &secondNode, 12);

    updateInfo.dropUnretainedNodes(updateInfo.planRetention(10), true);
    // A reader between the horizon and the newest commit still observes the older value.
    const Transaction midReader{TransactionType::READ_ONLY, 50, 7};
    std::vector<transaction_t> observed;
    updateInfo.iterateVectorInfo(&midReader, 0,
        [&](const VectorUpdateInfo& node) { observed.push_back(node.version); });
    ASSERT_EQ(observed.size(), 1u);
    EXPECT_EQ(observed[0], 5u);
}

TEST_F(MVCCRetentionTest, SyntheticBaseNodeSplicesBelowKeptNodes) {
    auto* mm = getMemoryManager(*database);
    UpdateInfo updateInfo;
    ValueVector values{LogicalType::INT64()};
    values.setState(std::make_shared<DataChunkState>(DEFAULT_VECTOR_CAPACITY));
    setIntValue(values, 200);
    Transaction write{*conn->getClientContext(), TransactionType::WRITE,
        Transaction::START_TRANSACTION_ID, 0};
    auto& node = updateInfo.update(*mm, &write, 0, 3, values);
    updateInfo.commit(0, &node, 12);

    updateInfo.dropUnretainedNodes(updateInfo.planRetention(10), true);
    auto& base = updateInfo.insertRetainedBaseNode(*mm, LogicalType::INT64(), 0, 10);
    EXPECT_EQ(base.version, 10u);
    // The synthetic node must not shadow the newer update it preserves the base for.
    const Transaction freshReader{TransactionType::READ_ONLY, 50, 30};
    std::vector<transaction_t> observed;
    updateInfo.iterateVectorInfo(&freshReader, 0,
        [&](const VectorUpdateInfo& info) { observed.push_back(info.version); });
    ASSERT_EQ(observed.size(), 2u);
    EXPECT_EQ(observed[0], 12u);
    EXPECT_EQ(observed[1], 10u);
}

} // namespace testing
} // namespace lbug

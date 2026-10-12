#include "common/serializer/buffer_reader.h"
#include "common/serializer/buffer_writer.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "graph_test/private_graph_test.h"
#include "main/connection.h"
#include "storage/buffer_manager/memory_manager.h"
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
    // Retained deletes round-trip with their stamps intact.
    EXPECT_FALSE(restored->isDeleted(&oldReader, 9));
    const Transaction newReader{TransactionType::READ_ONLY, 8, 14};
    EXPECT_TRUE(restored->isDeleted(&newReader, 9));
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

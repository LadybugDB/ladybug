#include <algorithm>
#include <vector>

#include "common/data_chunk/sel_vector.h"
#include "common/system_config.h"
#include "gtest/gtest.h"
#include "storage/table/version_info.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace {

constexpr auto WRITER_ID = Transaction::START_TRANSACTION_ID;
constexpr auto OTHER_WRITER_ID = WRITER_ID + 1;
constexpr auto READER_ID = WRITER_ID + 2;
constexpr transaction_t COMMIT_TS = 10;
constexpr auto HALF_VECTOR = DEFAULT_VECTOR_CAPACITY / 2;

std::vector<sel_t> selected(const SelectionVector& selVector) {
    const auto positions = selVector.getSelectedPositions();
    return {positions.begin(), positions.end()};
}

// Positions [start, end) without the ones in `skipped`.
std::vector<sel_t> positionsWithout(sel_t start, sel_t end, const std::vector<sel_t>& skipped) {
    std::vector<sel_t> positions;
    for (auto pos = start; pos < end; pos++) {
        if (std::find(skipped.begin(), skipped.end(), pos) == skipped.end()) {
            positions.push_back(pos);
        }
    }
    return positions;
}

} // namespace

TEST(VersionInfoScanTest, CommittedSingleWriterVectorStaysUnfiltered) {
    VersionInfo info;
    info.append(WRITER_ID, 0, DEFAULT_VECTOR_CAPACITY);
    info.commitInsert(0, DEFAULT_VECTOR_CAPACITY, COMMIT_TS);
    SelectionVector selVector{DEFAULT_VECTOR_CAPACITY};
    info.getSelVectorToScan(COMMIT_TS + 5, READER_ID, selVector, 0, DEFAULT_VECTOR_CAPACITY);
    EXPECT_TRUE(selVector.isUnfiltered());
    EXPECT_EQ(selVector.getSelSize(), DEFAULT_VECTOR_CAPACITY);
}

TEST(VersionInfoScanTest, ReaderOlderThanTheInsertSeesNoRows) {
    VersionInfo info;
    info.append(WRITER_ID, 0, DEFAULT_VECTOR_CAPACITY);
    info.commitInsert(0, DEFAULT_VECTOR_CAPACITY, COMMIT_TS);
    SelectionVector selVector{DEFAULT_VECTOR_CAPACITY};
    info.getSelVectorToScan(COMMIT_TS - 1, READER_ID, selVector, 0, DEFAULT_VECTOR_CAPACITY);
    EXPECT_EQ(selVector.getSelSize(), 0u);
}

TEST(VersionInfoScanTest, OnlyTheWriterSeesUncommittedRows) {
    VersionInfo info;
    info.append(WRITER_ID, 0, 100);
    SelectionVector selVector{DEFAULT_VECTOR_CAPACITY};
    info.getSelVectorToScan(COMMIT_TS, WRITER_ID, selVector, 0, 100);
    EXPECT_TRUE(selVector.isUnfiltered());
    EXPECT_EQ(selVector.getSelSize(), 100u);
    info.getSelVectorToScan(COMMIT_TS, READER_ID, selVector, 0, 100);
    EXPECT_EQ(selVector.getSelSize(), 0u);
}

TEST(VersionInfoScanTest, RolledBackInsertIsNotSelected) {
    VersionInfo info;
    info.append(WRITER_ID, 0, 100);
    info.rollbackInsert(0, 100);
    SelectionVector selVector{DEFAULT_VECTOR_CAPACITY};
    info.getSelVectorToScan(COMMIT_TS, WRITER_ID, selVector, 0, 100);
    EXPECT_EQ(selVector.getSelSize(), 0u);
}

// A delete on rows without version info marks their vector ALWAYS_INSERTED; once the delete is
// rolled back, the vector is selected whole again.
TEST(VersionInfoScanTest, AlwaysInsertedVectorIsSelectedWhole) {
    VersionInfo info;
    info.delete_(OTHER_WRITER_ID, 5);
    SelectionVector selVector{DEFAULT_VECTOR_CAPACITY};
    info.getSelVectorToScan(COMMIT_TS, OTHER_WRITER_ID, selVector, 0, DEFAULT_VECTOR_CAPACITY);
    EXPECT_EQ(selected(selVector), positionsWithout(0, DEFAULT_VECTOR_CAPACITY, {5}));
    info.rollbackDelete(5, 1);
    info.getSelVectorToScan(COMMIT_TS, READER_ID, selVector, 0, DEFAULT_VECTOR_CAPACITY);
    EXPECT_TRUE(selVector.isUnfiltered());
    EXPECT_EQ(selVector.getSelSize(), DEFAULT_VECTOR_CAPACITY);
}

// A checkpoint-built vector keeps per-row stamps: unstamped rows are visible to everyone, a stamped
// one only from its timestamp on.
TEST(VersionInfoScanTest, CheckpointStampsAreCheckedRowByRow) {
    VersionInfo info;
    info.stampInsertCommitTS(3, 50);
    SelectionVector selVector{DEFAULT_VECTOR_CAPACITY};
    info.getSelVectorToScan(40, READER_ID, selVector, 0, DEFAULT_VECTOR_CAPACITY);
    EXPECT_EQ(selected(selVector), positionsWithout(0, DEFAULT_VECTOR_CAPACITY, {3}));
    info.getSelVectorToScan(50, READER_ID, selVector, 0, DEFAULT_VECTOR_CAPACITY);
    EXPECT_EQ(selVector.getSelSize(), DEFAULT_VECTOR_CAPACITY);
}

TEST(VersionInfoScanTest, RowsFromTwoWritersAreCheckedRowByRow) {
    VersionInfo info;
    info.append(WRITER_ID, 0, HALF_VECTOR);
    info.commitInsert(0, HALF_VECTOR, COMMIT_TS);
    info.append(OTHER_WRITER_ID, HALF_VECTOR, HALF_VECTOR);
    info.commitInsert(HALF_VECTOR, HALF_VECTOR, COMMIT_TS + 10);
    SelectionVector selVector{DEFAULT_VECTOR_CAPACITY};
    info.getSelVectorToScan(COMMIT_TS + 5, READER_ID, selVector, 0, DEFAULT_VECTOR_CAPACITY);
    EXPECT_EQ(selected(selVector), positionsWithout(0, HALF_VECTOR, {}));
}

TEST(VersionInfoScanTest, DeletedRowsAreCheckedRowByRow) {
    VersionInfo info;
    info.append(WRITER_ID, 0, DEFAULT_VECTOR_CAPACITY);
    info.commitInsert(0, DEFAULT_VECTOR_CAPACITY, COMMIT_TS);
    info.delete_(OTHER_WRITER_ID, 7);
    info.commitDelete(7, 1, COMMIT_TS + 10);
    SelectionVector selVector{DEFAULT_VECTOR_CAPACITY};
    info.getSelVectorToScan(COMMIT_TS + 20, READER_ID, selVector, 0, DEFAULT_VECTOR_CAPACITY);
    EXPECT_EQ(selected(selVector), positionsWithout(0, DEFAULT_VECTOR_CAPACITY, {7}));
    // A reader older than the delete still sees the row.
    info.getSelVectorToScan(COMMIT_TS + 5, READER_ID, selVector, 0, DEFAULT_VECTOR_CAPACITY);
    EXPECT_EQ(selected(selVector), positionsWithout(0, DEFAULT_VECTOR_CAPACITY, {}));
}

// A scan range covering the end of one vector and the start of the next mixes the per-row and the
// whole-vector paths; positions must line up both ways round.
TEST(VersionInfoScanTest, WholeVectorAndRowCheckedVectorShareOneRange) {
    VersionInfo info;
    info.append(WRITER_ID, 0, 2 * DEFAULT_VECTOR_CAPACITY);
    info.commitInsert(0, 2 * DEFAULT_VECTOR_CAPACITY, COMMIT_TS);
    // Row-checked first vector: a delete in it.
    info.delete_(OTHER_WRITER_ID, HALF_VECTOR + 3);
    info.commitDelete(HALF_VECTOR + 3, 1, COMMIT_TS + 1);
    SelectionVector selVector{DEFAULT_VECTOR_CAPACITY};
    info.getSelVectorToScan(COMMIT_TS + 5, READER_ID, selVector, HALF_VECTOR,
        DEFAULT_VECTOR_CAPACITY);
    EXPECT_EQ(selected(selVector), positionsWithout(0, DEFAULT_VECTOR_CAPACITY, {3}));

    VersionInfo reversed;
    reversed.append(WRITER_ID, 0, 2 * DEFAULT_VECTOR_CAPACITY);
    reversed.commitInsert(0, 2 * DEFAULT_VECTOR_CAPACITY, COMMIT_TS);
    // Row-checked second vector: a delete in it.
    reversed.delete_(OTHER_WRITER_ID, DEFAULT_VECTOR_CAPACITY + 3);
    reversed.commitDelete(DEFAULT_VECTOR_CAPACITY + 3, 1, COMMIT_TS + 1);
    reversed.getSelVectorToScan(COMMIT_TS + 5, READER_ID, selVector, HALF_VECTOR,
        DEFAULT_VECTOR_CAPACITY);
    EXPECT_EQ(selected(selVector), positionsWithout(0, DEFAULT_VECTOR_CAPACITY, {HALF_VECTOR + 3}));
}

TEST(VersionInfoScanTest, LeadingVisibleRowsCoverCommittedVectors) {
    VersionInfo info;
    info.append(WRITER_ID, 0, DEFAULT_VECTOR_CAPACITY + HALF_VECTOR);
    info.commitInsert(0, DEFAULT_VECTOR_CAPACITY + HALF_VECTOR, COMMIT_TS);
    const Transaction reader{TransactionType::READ_ONLY, READER_ID, COMMIT_TS + 5};
    EXPECT_EQ(info.getNumLeadingVisibleRows(&reader, DEFAULT_VECTOR_CAPACITY + HALF_VECTOR),
        DEFAULT_VECTOR_CAPACITY + HALF_VECTOR);
    const Transaction olderReader{TransactionType::READ_ONLY, READER_ID, COMMIT_TS - 1};
    EXPECT_EQ(info.getNumLeadingVisibleRows(&olderReader, DEFAULT_VECTOR_CAPACITY + HALF_VECTOR),
        0u);
}

// The run stops at the first vector holding a row the reader does not see, so rows after it are
// left to the check per row.
TEST(VersionInfoScanTest, VisibleDeleteEndsTheLeadingRun) {
    VersionInfo info;
    const auto numRows = DEFAULT_VECTOR_CAPACITY * 2 + HALF_VECTOR;
    info.append(WRITER_ID, 0, numRows);
    info.commitInsert(0, numRows, COMMIT_TS);
    const auto deletedRow = DEFAULT_VECTOR_CAPACITY + HALF_VECTOR;
    ASSERT_TRUE(info.delete_(OTHER_WRITER_ID, deletedRow));
    // The deleter sees its own delete; another reader does not see it while it is uncommitted.
    const Transaction deleter{TransactionType::WRITE, OTHER_WRITER_ID, COMMIT_TS + 1};
    EXPECT_EQ(info.getNumLeadingVisibleRows(&deleter, numRows), DEFAULT_VECTOR_CAPACITY);
    const Transaction reader{TransactionType::READ_ONLY, READER_ID, COMMIT_TS + 5};
    EXPECT_EQ(info.getNumLeadingVisibleRows(&reader, numRows), numRows);
    info.commitDelete(deletedRow, 1, COMMIT_TS + 2);
    EXPECT_EQ(info.getNumLeadingVisibleRows(&reader, numRows), DEFAULT_VECTOR_CAPACITY);
    // A reader that started before the delete committed still sees every row.
    const Transaction olderReader{TransactionType::READ_ONLY, READER_ID, COMMIT_TS + 1};
    EXPECT_EQ(info.getNumLeadingVisibleRows(&olderReader, numRows), numRows);
}

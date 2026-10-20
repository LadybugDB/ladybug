#include <cstring>

#include "common/constants.h"
#include "gtest/gtest.h"
#include "main/connection.h"
#include "main/database.h"
#include "processor/result/factorized_table.h"
#include "storage/buffer_manager/memory_manager.h"

using namespace lbug;
using namespace lbug::processor;

namespace {

bool isFilled(const uint8_t* data, uint64_t size, uint8_t value) {
    for (auto i = 0u; i < size; i++) {
        if (data[i] != value) {
            return false;
        }
    }
    return true;
}

bool isZero(const uint8_t* data, uint64_t size) {
    return isFilled(data, size, 0);
}

FactorizedTableSchema makeSchema() {
    FactorizedTableSchema schema;
    schema.appendColumn(ColumnSchema(false /* isUnFlat */, 0 /* groupID */, 24));
    return schema;
}

void appendFilled(FactorizedTable& table, uint64_t numTuples, uint8_t value) {
    const auto numBytesPerTuple = table.getTableSchema()->getNumBytesPerTuple();
    for (auto i = 0u; i < numTuples; i++) {
        memset(table.appendEmptyTuple(), value, numBytesPerTuple);
    }
}

} // namespace

// Flat tuple blocks are zeroed as tuples are claimed rather than when the page is taken, so a
// recycled page must still hand out zeroed tuples, before and after clear().
TEST(FactorizedTableTest, ClaimedTuplesAreZeroOnRecycledPages) {
    main::Database db(":memory:");
    main::Connection conn(&db);
    auto* mm = storage::MemoryManager::Get(*conn.getClientContext());
    {
        auto dirty = mm->allocateBuffer(false, common::TEMP_PAGE_SIZE);
        memset(dirty->getBuffer().data(), 0xAB, dirty->getBuffer().size());
    }
    FactorizedTable table(mm, makeSchema());
    const auto numBytesPerTuple = table.getTableSchema()->getNumBytesPerTuple();
    // Spans several zeroing granules within the first block.
    const auto numTuples = 3 * 4096 / numBytesPerTuple + 7;
    for (auto round = 0; round < 2; round++) {
        for (auto i = 0u; i < numTuples; i++) {
            auto* tuple = table.appendEmptyTuple();
            ASSERT_TRUE(isZero(tuple, numBytesPerTuple)) << "round " << round << " tuple " << i;
            memset(tuple, 0xCD, numBytesPerTuple);
        }
        // Grows past the next 4 KB step, so resize itself has to zero.
        const auto numResized = numTuples + 4096 / numBytesPerTuple + 5;
        table.resize(numResized);
        for (auto i = numTuples; i < numResized; i++) {
            ASSERT_TRUE(isZero(table.getTuple(i), numBytesPerTuple))
                << "round " << round << " tuple " << i;
        }
        table.clear();
    }
}

// Shrinking gives tuples back to the block. Growing again, by resize or by appending, must hand
// them out zeroed rather than with their old bytes, and keep the tuples that were not given back.
TEST(FactorizedTableTest, TuplesAreZeroAfterShrinkAndRegrow) {
    main::Database db(":memory:");
    main::Connection conn(&db);
    auto* mm = storage::MemoryManager::Get(*conn.getClientContext());
    FactorizedTable table(mm, makeSchema());
    const auto numBytesPerTuple = table.getTableSchema()->getNumBytesPerTuple();
    // Spans several zeroing granules within the first block.
    const auto numTuples = 3 * 4096 / numBytesPerTuple + 7;
    const auto numKept = 5u;
    appendFilled(table, numTuples, 0xCD);
    table.resize(numKept);
    table.resize(numTuples);
    for (auto i = numKept; i < numTuples; i++) {
        ASSERT_TRUE(isZero(table.getTuple(i), numBytesPerTuple)) << "resize, tuple " << i;
        memset(table.getTuple(i), 0xCD, numBytesPerTuple);
    }
    table.resize(numKept);
    for (auto i = numKept; i < numTuples; i++) {
        ASSERT_TRUE(isZero(table.appendEmptyTuple(), numBytesPerTuple)) << "append, tuple " << i;
    }
    for (auto i = 0u; i < numKept; i++) {
        ASSERT_TRUE(isFilled(table.getTuple(i), numBytesPerTuple, 0xCD)) << i;
    }
}

// Merging copies the tuples of one table's last block into the other's last block, past the part
// that block had zeroed. Later claims must neither zero over the copied tuples nor hand out dirty
// bytes.
TEST(FactorizedTableTest, ClaimsAfterMergeKeepCopiedTuples) {
    main::Database db(":memory:");
    main::Connection conn(&db);
    auto* mm = storage::MemoryManager::Get(*conn.getClientContext());
    FactorizedTable target(mm, makeSchema());
    FactorizedTable source(mm, makeSchema());
    const auto numBytesPerTuple = target.getTableSchema()->getNumBytesPerTuple();
    // The source's tuples end just short of its first 4 KB step, and the target's tuples are
    // copied after them, across that step.
    const auto numSourceTuples = (4096 - 8) / numBytesPerTuple;
    const auto numTargetTuples = 100u;
    appendFilled(target, numTargetTuples, 0xA1);
    appendFilled(source, numSourceTuples, 0xB2);
    target.merge(source);
    ASSERT_EQ(target.getNumTuples(), numSourceTuples + numTargetTuples);
    for (auto i = 0u; i < numSourceTuples; i++) {
        ASSERT_TRUE(isFilled(target.getTuple(i), numBytesPerTuple, 0xB2)) << i;
    }
    auto* appended = target.appendEmptyTuple();
    ASSERT_TRUE(isZero(appended, numBytesPerTuple));
    for (auto i = numSourceTuples; i < numSourceTuples + numTargetTuples; i++) {
        ASSERT_TRUE(isFilled(target.getTuple(i), numBytesPerTuple, 0xA1)) << i;
    }
}

#include <limits>
#include <memory>
#include <string_view>

#include "common/exception/storage.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/table/dictionary_chunk.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace testing {
namespace {

using string_index_t = DictionaryChunk::string_index_t;
using string_offset_t = DictionaryChunk::string_offset_t;

class DictionaryOffsetValidationTest : public DBTest {
public:
    void SetUp() override {
        BaseGraphTest::SetUp(); // NOLINT
        createDBAndConn();
    }

    std::string getInputDir() override { return TestHelper::appendLbugRootPath("dataset/empty/"); }
};

// Build a small dictionary with two entries: "hello" (offset 0, len 5) and "world" (offset 5,
// len 5). Offsets are [0, 5], dataSize is 10.
std::unique_ptr<DictionaryChunk> buildHelloWorldDict(MemoryManager& mm) {
    auto dict = std::make_unique<DictionaryChunk>(mm, 8 /*capacity*/, true /*enableCompression*/,
        ResidencyState::IN_MEMORY);
    dict->appendString("hello");
    dict->appendString("world");
    return dict;
}

TEST_F(DictionaryOffsetValidationTest, ValidDictionaryReadsSucceed) {
    auto* mm = getMemoryManager(*database);
    auto dict = buildHelloWorldDict(*mm);
    EXPECT_TRUE(dict->sanityCheck());
    EXPECT_EQ(dict->getStringLength(0), 5u);
    EXPECT_EQ(dict->getStringLength(1), 5u);
    EXPECT_EQ(dict->getString(0), std::string_view("hello"));
    EXPECT_EQ(dict->getString(1), std::string_view("world"));
}

// Reproduces https://github.com/LadybugDB/ladybug/issues/992: persisted offsets such as
// offset[2]=219, offset[3]=174 are non-monotonic. Before the fix this underflowed into a huge
// unsigned length and crashed in std::_Hash_bytes during checkpoint.
TEST_F(DictionaryOffsetValidationTest, NonMonotonicOffsetsThrowInsteadOfUnderflowing) {
    auto* mm = getMemoryManager(*database);
    auto dict = buildHelloWorldDict(*mm);
    // Corrupt [0, 5] into the decreasing pair [8, 3]: entry 0 spans [8, 3), which underflowed
    // to 0xffffffffffffffd3 before the fix.
    dict->getOffsetChunk()->setValue<string_offset_t>(8, 0);
    dict->getOffsetChunk()->setValue<string_offset_t>(3, 1);
    EXPECT_FALSE(dict->sanityCheck());
    EXPECT_THROW(dict->getStringLength(0), StorageException);
    EXPECT_THROW(dict->getString(0), StorageException);
}

TEST_F(DictionaryOffsetValidationTest, OutOfRangeOffsetThrows) {
    auto* mm = getMemoryManager(*database);
    auto dict = buildHelloWorldDict(*mm);
    const auto dataSize = dict->getStringDataChunk()->getNumValues();
    ASSERT_EQ(dataSize, 10u);
    // Point the last entry past the end of the data chunk.
    dict->getOffsetChunk()->setValue<string_offset_t>(dataSize + 100, 1);
    EXPECT_FALSE(dict->sanityCheck());
    EXPECT_THROW(dict->getStringLength(1), StorageException);
    EXPECT_THROW(dict->getString(1), StorageException);
}

TEST_F(DictionaryOffsetValidationTest, OutOfBoundsIndexThrows) {
    auto* mm = getMemoryManager(*database);
    auto dict = buildHelloWorldDict(*mm);
    const auto numOffsets = dict->getOffsetChunk()->getNumValues();
    ASSERT_EQ(numOffsets, 2u);
    EXPECT_THROW(dict->getStringLength(static_cast<string_index_t>(numOffsets)), StorageException);
    EXPECT_THROW(dict->getString(static_cast<string_index_t>(numOffsets)), StorageException);
    EXPECT_THROW(dict->getStringLength(std::numeric_limits<string_index_t>::max()),
        StorageException);
}

TEST_F(DictionaryOffsetValidationTest, EmptyDictionaryIndexThrows) {
    auto* mm = getMemoryManager(*database);
    DictionaryChunk dict(*mm, 8 /*capacity*/, true /*enableCompression*/,
        ResidencyState::IN_MEMORY);
    ASSERT_EQ(dict.getOffsetChunk()->getNumValues(), 0u);
    EXPECT_TRUE(dict.sanityCheck());
    EXPECT_THROW(dict.getStringLength(0), StorageException);
    EXPECT_THROW(dict.getString(0), StorageException);
}

TEST_F(DictionaryOffsetValidationTest, CorruptDictionaryHashThrowsInsteadOfSegfault) {
    auto* mm = getMemoryManager(*database);
    auto dict = buildHelloWorldDict(*mm);
    // Same corruption as the checkpoint path in #992: looking up an existing entry hashes and
    // compares against corrupt dictionary bytes (inside appendString's indexTable.find), which
    // must throw rather than read out of bounds. Note the lookup only touches entries in the
    // same hash bucket, so re-appending a known duplicate deterministically hits the corrupt
    // entry, while a fresh key may hash to an empty bucket and take the append path.
    dict->getOffsetChunk()->setValue<string_offset_t>(8, 0);
    dict->getOffsetChunk()->setValue<string_offset_t>(3, 1);
    EXPECT_THROW(dict->appendString("hello"), StorageException);
}

} // namespace
} // namespace testing
} // namespace lbug

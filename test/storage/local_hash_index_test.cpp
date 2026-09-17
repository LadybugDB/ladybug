#include "common/exception/runtime.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "storage/buffer_manager/buffer_manager.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/disk_array_collection.h"
#include "storage/index/hash_index.h"
#include "storage/index/hash_index_header.h"
#include "storage/local_storage/local_hash_index.h"
#include "storage/overflow_file.h"
#include "storage/storage_manager.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::storage;

bool isVisible(offset_t) {
    return true;
}

TEST(LocalHashIndexTests, LocalInserts) {
    BufferManager bm(":memory:", "", 256 * 1024 * 1024 /*bufferPoolSize*/,
        512 * 1024 * 1024 /*maxDBSize*/, nullptr, true);
    MemoryManager memoryManager(&bm, nullptr);
    auto overflowFile = std::make_unique<InMemOverflowFile>(memoryManager);
    auto* overflowFileHandle = overflowFile->addHandle();
    auto hashIndex =
        std::make_unique<LocalHashIndex>(memoryManager, PhysicalTypeID::INT64, overflowFileHandle);

    for (int64_t i = 0u; i < 100000; i++) {
        ASSERT_TRUE(hashIndex->insert(i, i * 2, isVisible));
    }
    for (int64_t i = 0u; i < 100000; i++) {
        ASSERT_FALSE(hashIndex->insert(i, i, isVisible));
    }

    for (int64_t i = 0u; i < 100000; i++) {
        hashIndex->delete_(i);
    }
    for (int64_t i = 0u; i < 100000; i++) {
        ASSERT_TRUE(hashIndex->insert(i, i, isVisible));
    }
}

std::string gen_random(const int len) {
    static const char alphanum[] = "0123456789"
                                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                   "abcdefghijklmnopqrstuvwxyz";
    std::string tmp_s;
    tmp_s.reserve(len);

    for (int i = 0; i < len; ++i) {
        tmp_s += alphanum[rand() % (sizeof(alphanum) - 1)];
    }

    return tmp_s;
}

TEST(LocalHashIndexTests, LocalStringInserts) {
    BufferManager bm(":memory:", "", 256 * 1024 * 1024 /*bufferPoolSize*/,
        512 * 1024 * 1024 /*maxDBSize*/, nullptr, true);
    MemoryManager memoryManager(&bm, nullptr);
    auto overflowFile = std::make_unique<InMemOverflowFile>(memoryManager);
    auto* overflowFileHandle = overflowFile->addHandle();
    auto hashIndex =
        std::make_unique<LocalHashIndex>(memoryManager, PhysicalTypeID::STRING, overflowFileHandle);

    std::vector<std::string> keys;
    for (int64_t i = 0u; i < 100; i++) {
        keys.push_back(gen_random(14));
        ASSERT_TRUE(hashIndex->insert(keys.back(), i * 2, isVisible));
    }
    for (int64_t i = 0u; i < 100; i++) {
        ASSERT_FALSE(hashIndex->insert(keys[i], i * 2, isVisible));
    }
}

TEST(HashIndexHeaderTests, RejectsCorruptedOnDiskHeader) {
    HashIndexHeaderOnDisk header;
    header.currentLevel = 64;
    EXPECT_THROW(HashIndexHeader{header}, RuntimeException);

    header.currentLevel = 4;
    header.nextSplitSlotId = 16;
    EXPECT_THROW(HashIndexHeader{header}, RuntimeException);
}

// Exercises the HashIndex load-time validation: a header that passes the standalone
// header check must still be consistent with the slot arrays present in the file.
// Corrupted values here must fail fast instead of driving huge allocations (issue #403).
namespace lbug {
namespace testing {

class HashIndexLoadTest : public EmptyDBTest {
public:
    void SetUp() override {
        BaseGraphTest::SetUp();
        createDBAndConn();
    }

    std::string getInputDir() override { UNREACHABLE_CODE; }

protected:
    using SlotType = HashIndex<int64_t>::OnDiskSlotType;

    static HashIndexHeaderOnDisk makeOnDiskHeader(uint8_t level, slot_id_t nextSplitSlotId,
        uint64_t numEntries) {
        HashIndexHeaderOnDisk header;
        header.currentLevel = level;
        header.nextSplitSlotId = nextSplitSlotId;
        header.numEntries = numEntries;
        return header;
    }

    // Builds a disk array collection with empty slot arrays, optionally grows the
    // primary slot array, then loads a HashIndex from the given header (may throw).
    void loadIndex(const HashIndexHeaderOnDisk& onDiskHeader, uint64_t numPrimarySlots) {
        BufferManager bm(":memory:", "", 256 * 1024 * 1024 /*bufferPoolSize*/,
            512 * 1024 * 1024 /*maxDBSize*/, nullptr, true);
        MemoryManager memoryManager(&bm, nullptr);
        auto* storageManager = getStorageManager(*database);
        DiskArrayCollection diskArrays(*storageManager->getDataFH(),
            storageManager->getShadowFile(), true /*bypassShadowing*/);
        for (size_t i = 0; i < NUM_HASH_INDEXES * 2; i++) {
            diskArrays.addDiskArray();
        }
        if (numPrimarySlots > 0) {
            auto pSlots = diskArrays.getDiskArray<SlotType>(0);
            pSlots->resize(*storageManager->getDataFH()->getPageManager(),
                &transaction::DUMMY_CHECKPOINT_TRANSACTION, numPrimarySlots);
        }
        HashIndexHeader readHeader{onDiskHeader};
        HashIndexHeader writeHeader = readHeader;
        HashIndex<int64_t> index(memoryManager, nullptr /*overflowFileHandle*/, diskArrays,
            0 /*indexPos*/, &storageManager->getShadowFile(), readHeader, writeHeader);
    }
};

TEST_F(HashIndexLoadTest, LoadsHealthyEmptyIndex) {
    EXPECT_NO_THROW(loadIndex(makeOnDiskHeader(1, 0, 0), 0));
}

TEST_F(HashIndexLoadTest, RejectsHugeNumEntries) {
    // Value from issue #403: must throw instead of OOMing the process.
    EXPECT_THROW(loadIndex(makeOnDiskHeader(1, 0, 16544419524162413700ull), 0), RuntimeException);
}

TEST_F(HashIndexLoadTest, RejectsEmptyIndexWithNonDefaultLevel) {
    EXPECT_THROW(loadIndex(makeOnDiskHeader(4, 0, 0), 0), RuntimeException);
}

TEST_F(HashIndexLoadTest, RejectsLevelRequiringMoreSlotsThanPresent) {
    // Level 4 needs 16 primary slots, but only 2 exist.
    EXPECT_THROW(loadIndex(makeOnDiskHeader(4, 0, 0), 2), RuntimeException);
}

TEST_F(HashIndexLoadTest, RejectsSplitSlotIdBeyondSlotCount) {
    // Level 1 with 2 primary slots allows nextSplitSlotId 0 only (2 - 2 = 0 slots split so far).
    EXPECT_THROW(loadIndex(makeOnDiskHeader(1, 1, 0), 2), RuntimeException);
}

} // namespace testing
} // namespace lbug

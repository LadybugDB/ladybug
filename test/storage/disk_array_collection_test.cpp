#include <array>
#include <cstring>

#include "common/constants.h"
#include "common/exception/runtime.h"
#include "common/type_utils.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "storage/disk_array.h"
#include "storage/disk_array_collection.h"
#include "storage/enums/page_read_policy.h"
#include "storage/overflow_file.h"
#include "storage/page_manager.h"
#include "storage/storage_manager.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace testing {

class DiskArrayCollectionTest : public EmptyDBTest {
public:
    void SetUp() override {
        BaseGraphTest::SetUp();
        createDBAndConn();
    }

    std::string getInputDir() override { UNREACHABLE_CODE; }

protected:
    static constexpr size_t NUM_HEADERS_PER_PAGE =
        (LBUG_PAGE_SIZE - sizeof(page_idx_t) - sizeof(uint32_t)) / sizeof(DiskArrayHeader);
    static constexpr size_t NEXT_HEADER_PAGE_OFFSET =
        NUM_HEADERS_PER_PAGE * sizeof(DiskArrayHeader);

    page_idx_t allocateHeaderPage() {
        return getStorageManager(*database)->getDataFH()->getPageManager()->allocatePage();
    }

    void writeHeaderPage(page_idx_t page, page_idx_t nextPage, uint32_t numHeaders) {
        auto* fileHandle = getStorageManager(*database)->getDataFH();
        uint8_t* frame;
        if (inMemMode) {
            frame = fileHandle->getFrame(page);
        } else {
            frame = fileHandle->pinPage(page, PageReadPolicy::DONT_READ_PAGE);
        }
        std::memset(frame, 0, LBUG_PAGE_SIZE);
        std::memcpy(frame + NEXT_HEADER_PAGE_OFFSET, &nextPage, sizeof(nextPage));
        std::memcpy(frame + NEXT_HEADER_PAGE_OFFSET + sizeof(nextPage), &numHeaders,
            sizeof(numHeaders));
        fileHandle->setLockedPageDirty(page);
        if (!inMemMode) {
            fileHandle->unpinPage(page);
        }
    }

    // Writes a header page that holds a single disk array with the given header.
    void writeHeaderPage(page_idx_t page, const DiskArrayHeader& header) {
        writeHeaderPage(page, INVALID_PAGE_IDX, 1);
        auto* fileHandle = getStorageManager(*database)->getDataFH();
        auto* frame = fileHandle->pinPage(page, PageReadPolicy::READ_PAGE);
        std::memcpy(frame, &header, sizeof(header));
        fileHandle->setLockedPageDirty(page);
        fileHandle->unpinPage(page);
    }

    // PIPs are read directly from the file, so they are written directly to it too.
    void writePIP(page_idx_t page, const PIP& pip) {
        getStorageManager(*database)->getDataFH()->writePageToFile(
            reinterpret_cast<const uint8_t*>(&pip), page);
    }

    void expectCorruptedDiskArray(uint64_t numElements, page_idx_t pipPage) {
        const auto headerPage = allocateHeaderPage();
        DiskArrayHeader header;
        header.numElements = numElements;
        header.firstPIPPageIdx = pipPage;
        writeHeaderPage(headerPage, header);
        auto* storageManager = getStorageManager(*database);
        DiskArrayCollection diskArrays(*storageManager->getDataFH(),
            storageManager->getShadowFile(), headerPage, true);
        EXPECT_THROW(diskArrays.getDiskArray<uint64_t>(0), RuntimeException);
    }

    void expectCorruption(page_idx_t firstHeaderPage) {
        auto* storageManager = getStorageManager(*database);
        EXPECT_THROW(
            [&] {
                DiskArrayCollection diskArrays(*storageManager->getDataFH(),
                    storageManager->getShadowFile(), firstHeaderPage, true);
            }(),
            RuntimeException);
    }
};

TEST_F(DiskArrayCollectionTest, RejectsCorruptedHeaderPageChain) {
    auto* fileHandle = getStorageManager(*database)->getDataFH();

    expectCorruption(0);
    expectCorruption(INVALID_PAGE_IDX);
    expectCorruption(fileHandle->getNumPages() + 1);

    const auto selfLoopPage = allocateHeaderPage();
    writeHeaderPage(selfLoopPage, selfLoopPage, 0);
    expectCorruption(selfLoopPage);

    const auto cyclePage1 = allocateHeaderPage();
    const auto cyclePage2 = allocateHeaderPage();
    writeHeaderPage(cyclePage1, cyclePage2, 0);
    writeHeaderPage(cyclePage2, cyclePage1, 0);
    expectCorruption(cyclePage1);

    const auto tooManyHeadersPage = allocateHeaderPage();
    writeHeaderPage(tooManyHeadersPage, INVALID_PAGE_IDX, NUM_HEADERS_PER_PAGE + 1);
    expectCorruption(tooManyHeadersPage);
}

TEST_F(DiskArrayCollectionTest, RejectsDiskArrayIndexOutsideHeaderCount) {
    const auto headerPage = allocateHeaderPage();
    writeHeaderPage(headerPage, INVALID_PAGE_IDX, 0);
    auto* storageManager = getStorageManager(*database);
    DiskArrayCollection diskArrays(*storageManager->getDataFH(), storageManager->getShadowFile(),
        headerPage, true);

    EXPECT_THROW(diskArrays.getDiskArray<uint64_t>(0), RuntimeException);
}

// Array pages are read lazily through the buffer manager, which does not bounds-check page
// indices: a corrupted PIP must be rejected when the disk array is loaded.
TEST_F(DiskArrayCollectionTest, RejectsCorruptedPageIndexPages) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    static constexpr uint64_t NUM_ELEMENTS_PER_PAGE = LBUG_PAGE_SIZE / sizeof(uint64_t);
    auto* fileHandle = getStorageManager(*database)->getDataFH();

    // The only array page is stored beyond the end of the file (e.g. the PIP was overwritten).
    const auto outOfBoundsPIP = allocateHeaderPage();
    PIP pip;
    std::memset(&pip, 0xFF, sizeof(pip));
    writePIP(outOfBoundsPIP, pip);
    expectCorruptedDiskArray(1, outOfBoundsPIP);

    // The header claims more array pages than the PIP chain addresses.
    const auto arrayPage = allocateHeaderPage();
    const auto shortPIP = allocateHeaderPage();
    pip = PIP();
    pip.pageIdxs[0] = arrayPage;
    writePIP(shortPIP, pip);
    expectCorruptedDiskArray((NUM_PAGE_IDXS_PER_PIP + 1) * NUM_ELEMENTS_PER_PAGE, shortPIP);
    // ... or than the PIP has valid entries for.
    expectCorruptedDiskArray(NUM_ELEMENTS_PER_PAGE + 1, shortPIP);

    // The PIP chain loops back on itself.
    const auto cyclePIP1 = allocateHeaderPage();
    const auto cyclePIP2 = allocateHeaderPage();
    pip.nextPipPageIdx = cyclePIP2;
    writePIP(cyclePIP1, pip);
    pip.nextPipPageIdx = cyclePIP1;
    writePIP(cyclePIP2, pip);
    expectCorruptedDiskArray(1, cyclePIP1);

    // ... or points beyond the end of the file.
    pip.nextPipPageIdx = fileHandle->getNumPages() + 1;
    writePIP(cyclePIP1, pip);
    expectCorruptedDiskArray(1, cyclePIP1);
}

// Element indices can come from on-disk data (e.g. a hash index slot's overflow slot id).
TEST_F(DiskArrayCollectionTest, RejectsElementIndexOutsideDiskArray) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    static constexpr uint64_t NUM_ELEMENTS = 3;
    auto* storageManager = getStorageManager(*database);
    auto* fileHandle = storageManager->getDataFH();
    const auto arrayPage = allocateHeaderPage();
    std::array<uint64_t, LBUG_PAGE_SIZE / sizeof(uint64_t)> elements{};
    elements[NUM_ELEMENTS - 1] = 42;
    fileHandle->writePageToFile(reinterpret_cast<const uint8_t*>(elements.data()), arrayPage);
    const auto pipPage = allocateHeaderPage();
    PIP pip;
    pip.pageIdxs[0] = arrayPage;
    writePIP(pipPage, pip);
    const auto headerPage = allocateHeaderPage();
    DiskArrayHeader header;
    header.numElements = NUM_ELEMENTS;
    header.firstPIPPageIdx = pipPage;
    writeHeaderPage(headerPage, header);

    DiskArrayCollection diskArrays(*fileHandle, storageManager->getShadowFile(), headerPage, true);
    const auto diskArray = diskArrays.getDiskArray<uint64_t>(0);
    EXPECT_EQ(diskArray->get(NUM_ELEMENTS - 1, &transaction::DUMMY_TRANSACTION), 42);
    EXPECT_THROW(diskArray->get(NUM_ELEMENTS, &transaction::DUMMY_TRANSACTION), RuntimeException);
    EXPECT_THROW(
        diskArray->get(LBUG_PAGE_SIZE * NUM_PAGE_IDXS_PER_PIP, &transaction::DUMMY_TRANSACTION),
        RuntimeException);
}

// Overflow pointers of long string keys are stored in hash index slots.
TEST_F(DiskArrayCollectionTest, RejectsOverflowPointerOutsideFile) {
    if (inMemMode) {
        GTEST_SKIP();
    }
    auto* storageManager = getStorageManager(*database);
    auto* fileHandle = storageManager->getDataFH();
    OverflowFile overflowFile(fileHandle, *getMemoryManager(*database),
        &storageManager->getShadowFile(), INVALID_PAGE_IDX);
    const auto* handle = overflowFile.addHandle();
    const std::string key(string_t::SHORT_STR_LENGTH + 1, 'a');
    string_t corruptedKey;
    corruptedKey.len = key.size();
    std::memcpy(corruptedKey.prefix, key.data(), string_t::PREFIX_LENGTH);
    for (const auto pageIdx : {fileHandle->getNumPages(), INVALID_PAGE_IDX - 1}) {
        TypeUtils::encodeOverflowPtr(corruptedKey.overflowPtr, pageIdx, 0);
        EXPECT_THROW(handle->equals(transaction::TransactionType::READ_ONLY, key, corruptedKey),
            RuntimeException);
        EXPECT_THROW(handle->readString(transaction::TransactionType::READ_ONLY, corruptedKey),
            RuntimeException);
    }
}

} // namespace testing
} // namespace lbug

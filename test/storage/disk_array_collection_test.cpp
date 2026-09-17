#include <cstring>

#include "common/constants.h"
#include "common/exception/runtime.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "storage/disk_array.h"
#include "storage/disk_array_collection.h"
#include "storage/enums/page_read_policy.h"
#include "storage/page_manager.h"
#include "storage/storage_manager.h"

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

} // namespace testing
} // namespace lbug

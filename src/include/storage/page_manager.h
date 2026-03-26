#pragma once

#include <atomic>
#include <mutex>

#include "common/types/types.h"
#include "storage/free_space_manager.h"
#include "storage/page_allocator.h"
#include "storage/page_range.h"

namespace lbug {
namespace transaction {
enum class TransactionType : uint8_t;
}
namespace storage {
struct PageCursor;
struct DBFileID;
class PageManager;
class FileHandle;

class PageManager : public PageAllocator {
public:
    explicit PageManager(FileHandle* fileHandle)
        : PageAllocator(fileHandle), freeSpaceManager(std::make_unique<FreeSpaceManager>()),
          fileHandle(fileHandle), version(0) {}

    uint64_t getVersion() const { return version.load(std::memory_order_relaxed); }
    bool changedSinceLastCheckpoint() const {
        return version.load(std::memory_order_relaxed) != lastCheckpointVersion;
    }
    void resetVersion() { lastCheckpointVersion = version.load(std::memory_order_relaxed); }
    void resetVersion(uint64_t checkpointedVersion) { lastCheckpointVersion = checkpointedVersion; }

    PageRange allocatePageRange(common::page_idx_t numPages) override;
    void freePageRange(PageRange block) override;
    void freeImmediatelyRewritablePageRange(FileHandle* fileHandle, PageRange block);

    // The page manager must first allocate space for itself so that its serialized version also
    // tracks the pages allocated itself
    // Thus this function also allocates and returns the space for the serialized storage maanger
    common::page_idx_t estimatePagesNeededForSerialize();
    void serialize(common::Serializer& serializer);
    void deserialize(common::Deserializer& deSer);
    void finalizeCheckpoint();
    void rollbackCheckpoint() { freeSpaceManager->rollbackCheckpoint(); }

    common::row_idx_t getNumFreeEntries() const { return freeSpaceManager->getNumEntries(); }
    std::vector<PageRange> getFreeEntries(common::row_idx_t startOffset,
        common::row_idx_t endOffset) const {
        return freeSpaceManager->getEntries(startOffset, endOffset);
    }

    void clearEvictedBMEntriesIfNeeded(BufferManager* bufferManager);
    void mergeFreePages(FileHandle* fileHandle);
    void reclaimTailPagesIfNeeded(common::page_idx_t checkpointNumPages);

    static PageManager* Get(const main::ClientContext& context);

private:
    std::unique_ptr<FreeSpaceManager> freeSpaceManager;
    std::mutex mtx;
    FileHandle* fileHandle;
    std::atomic<uint64_t> version;
    uint64_t lastCheckpointVersion = 0;
};
} // namespace storage
} // namespace lbug

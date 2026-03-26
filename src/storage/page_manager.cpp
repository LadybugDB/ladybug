#include "storage/page_manager.h"

#include "common/uniq_lock.h"
#include "storage/file_handle.h"
#include "storage/storage_manager.h"

namespace lbug::storage {
static constexpr bool ENABLE_FSM = true;

PageRange PageManager::allocatePageRange(common::page_idx_t numPages) {
    if constexpr (ENABLE_FSM) {
        common::UniqLock lck{mtx};
        auto allocatedFreeChunk = freeSpaceManager->popFreePages(numPages);
        if (allocatedFreeChunk.has_value()) {
            version.fetch_add(1, std::memory_order_relaxed);
            return {*allocatedFreeChunk};
        }
    }
    auto startPageIdx = fileHandle->addNewPages(numPages);
    DASSERT(fileHandle->getNumPages() >= startPageIdx + numPages);
    return PageRange(startPageIdx, numPages);
}

void PageManager::freePageRange(PageRange entry) {
    if constexpr (ENABLE_FSM) {
        common::UniqLock lck{mtx};
        // Freed pages cannot be immediately reused to ensure checkpoint recovery works
        // Instead they are reusable after the end of the next checkpoint
        freeSpaceManager->addUncheckpointedFreePages(entry);
        version.fetch_add(1, std::memory_order_relaxed);
    }
}

common::page_idx_t PageManager::estimatePagesNeededForSerialize() {
    return freeSpaceManager->getMaxNumPagesForSerialization();
}

void PageManager::freeImmediatelyRewritablePageRange(FileHandle* fileHandle, PageRange entry) {
    if constexpr (ENABLE_FSM) {
        common::UniqLock lck{mtx};
        freeSpaceManager->evictAndAddFreePages(fileHandle, entry);
        version.fetch_add(1, std::memory_order_relaxed);
    }
}

void PageManager::serialize(common::Serializer& serializer) {
    freeSpaceManager->serialize(serializer);
}

void PageManager::deserialize(common::Deserializer& deSer) {
    freeSpaceManager->deserialize(deSer);
}

void PageManager::finalizeCheckpoint() {
    common::UniqLock lck{mtx};
    freeSpaceManager->finalizeCheckpoint(fileHandle);
}

void PageManager::clearEvictedBMEntriesIfNeeded(BufferManager* bufferManager) {
    freeSpaceManager->clearEvictedBufferManagerEntriesIfNeeded(bufferManager);
}

void PageManager::mergeFreePages(FileHandle* fileHandle) {
    if constexpr (ENABLE_FSM) {
        common::UniqLock lck{mtx};
        freeSpaceManager->mergeFreePages(fileHandle);
        version.fetch_add(1, std::memory_order_relaxed);
    }
}

void PageManager::reclaimTailPagesIfNeeded(common::page_idx_t checkpointNumPages) {
    if constexpr (!ENABLE_FSM) {
        return;
    }
    if (checkpointNumPages == 0) {
        return;
    }
    const auto currentNumPages = fileHandle->getNumPages();
    if (currentNumPages <= checkpointNumPages) {
        return;
    }
    common::UniqLock lck{mtx};
    const PageRange tail(checkpointNumPages, currentNumPages - checkpointNumPages);
    freeSpaceManager->evictAndAddFreePages(fileHandle, tail);
    version.fetch_add(1, std::memory_order_relaxed);
}

PageManager* PageManager::Get(const main::ClientContext& context) {
    return StorageManager::Get(context)->getDataFH()->getPageManager();
}

} // namespace lbug::storage

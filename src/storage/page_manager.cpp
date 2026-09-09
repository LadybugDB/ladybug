#include "storage/page_manager.h"

#include "common/exception/runtime.h"
#include "common/uniq_lock.h"
#include "storage/file_handle.h"
#include "storage/storage_manager.h"
#include <format>

namespace lbug::storage {
static constexpr bool ENABLE_FSM = true;

namespace {
// An allocated page index is turned straight into a file offset by the write path, so a range
// that does not belong to this file silently extends it rather than failing (issue #948: page
// 233,877,254 of a 2884-page database left an 11.8 MB file reporting 892 GiB of apparent size,
// which only surfaces once a copy or backup materialises the sparse hole). The free list is
// deserialized from the database file, so a corrupted entry lands here first: fail at the
// source, where the offending range can still be named.
//
// The file does shrink, via FreeSpaceManager::handleLastPageRange ->
// FileHandle::removePageIdxAndTruncateIfNecessary, but that path truncates to the start of the
// trailing free range and drops that range instead of re-adding it, and every surviving entry
// sorts below it, so a legitimate free entry stays inside the file.
void validateAllocatedPageRange(const PageRange& range, const FileHandle& fileHandle,
    const char* source) {
    if (range.numPages == 0) {
        // A zero-page allocation writes nothing, and its start index legitimately sits at the
        // current end of the file.
        return;
    }
    const auto numPages = fileHandle.getNumPages();
    if (range.startPageIdx >= numPages || range.numPages > numPages - range.startPageIdx) {
        throw common::RuntimeException(
            std::format("Page allocation from {} returned pages [{}, {}), which are out of "
                        "bounds for a data file with {} pages. The database file may be "
                        "corrupted.",
                source, range.startPageIdx,
                static_cast<uint64_t>(range.startPageIdx) + range.numPages, numPages));
    }
}
} // namespace

PageRange PageManager::allocatePageRange(common::page_idx_t numPages) {
    if constexpr (ENABLE_FSM) {
        common::UniqLock lck{mtx};
        auto allocatedFreeChunk = freeSpaceManager->popFreePages(numPages);
        if (allocatedFreeChunk.has_value()) {
            validateAllocatedPageRange(*allocatedFreeChunk, *fileHandle, "the free page list");
            version.fetch_add(1, std::memory_order_relaxed);
            return {*allocatedFreeChunk};
        }
    }
    auto startPageIdx = fileHandle->addNewPages(numPages);
    // DASSERT alone is stripped in release builds, and this invariant guards a file offset.
    DASSERT(fileHandle->getNumPages() >= startPageIdx + numPages);
    validateAllocatedPageRange(PageRange(startPageIdx, numPages), *fileHandle,
        "the page extension path");
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
    // Tail pages are beyond the checkpoint boundary. Add them as uncheckpointed first and merge
    // with the deserialized FSM state so we don't leave overlapping entries after recovery.
    freeSpaceManager->addUncheckpointedFreePages(tail);
    freeSpaceManager->mergeFreePages(fileHandle);
    version.fetch_add(1, std::memory_order_relaxed);
}

PageManager* PageManager::Get(const main::ClientContext& context) {
    return StorageManager::Get(context)->getDataFH()->getPageManager();
}

} // namespace lbug::storage

#include "storage/optimistic_allocator.h"

#include <algorithm>

#include "storage/page_manager.h"

namespace lbug::storage {
OptimisticAllocator::OptimisticAllocator(PageManager& pageManager)
    : PageAllocator(pageManager.getDataFH()), pageManager(pageManager) {}

PageRange OptimisticAllocator::allocatePageRange(common::page_idx_t numPages) {
    auto pageRange = pageManager.allocatePageRange(numPages);
    if (numPages > 0) {
        std::unique_lock lck{mtx};
        optimisticallyAllocatedPages.push_back(pageRange);
    }
    return pageRange;
}

void OptimisticAllocator::freePageRange(PageRange block) {
    pageManager.freePageRange(block);
}

void OptimisticAllocator::rollback() {
    std::unique_lock lck{mtx};
    for (const auto& entry : optimisticallyAllocatedPages) {
        pageManager.freeImmediatelyRewritablePageRange(pageManager.getDataFH(), entry);
    }
    optimisticallyAllocatedPages.clear();
}

void OptimisticAllocator::commit() {
    std::unique_lock lck{mtx};
    optimisticallyAllocatedPages.clear();
}

void OptimisticAllocator::retainPageRange(PageRange block) {
    if (block.numPages == 0) {
        return;
    }
    std::unique_lock lck{mtx};
    // Keep only the parts still owned by rollback. The group traversal may also visit
    // another writer's pages or already-retained pages, so a miss is normal.
    const auto retainedEnd = uint64_t{block.startPageIdx} + block.numPages;
    for (size_t i = 0; i < optimisticallyAllocatedPages.size(); ++i) {
        auto& entry = optimisticallyAllocatedPages[i];
        const auto entryEnd = uint64_t{entry.startPageIdx} + entry.numPages;
        if (retainedEnd <= entry.startPageIdx || block.startPageIdx >= entryEnd) {
            continue;
        }
        if (block.startPageIdx <= entry.startPageIdx) {
            const auto cutEnd = std::min(retainedEnd, entryEnd);
            entry.startPageIdx = static_cast<common::page_idx_t>(cutEnd);
            entry.numPages = static_cast<common::page_idx_t>(entryEnd - cutEnd);
        } else {
            if (retainedEnd < entryEnd) {
                optimisticallyAllocatedPages.emplace_back(
                    static_cast<common::page_idx_t>(retainedEnd),
                    static_cast<common::page_idx_t>(entryEnd - retainedEnd));
            }
            // emplace_back may have invalidated entry.
            auto& left = optimisticallyAllocatedPages[i];
            left.numPages = block.startPageIdx - left.startPageIdx;
        }
    }
    std::erase_if(optimisticallyAllocatedPages,
        [](const auto& entry) { return entry.numPages == 0; });
    // A repeated or foreign range can miss, but rollback must never still own any of it.
    DASSERT(std::ranges::none_of(optimisticallyAllocatedPages, [&](const auto& entry) {
        return uint64_t{entry.startPageIdx} + entry.numPages > block.startPageIdx &&
               entry.startPageIdx < retainedEnd;
    }));
}
} // namespace lbug::storage

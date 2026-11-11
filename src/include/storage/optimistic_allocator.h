#pragma once

#include <mutex>
#include <vector>

#include "storage/page_allocator.h"

namespace lbug {
namespace storage {

class PageManager;

/**
 * Manages any optimistically allocated pages (e.g. during COPY) so that they can be freed if a
 * rollback occurs.
 * Instances are shared across worker threads (LocalStorage caches one allocator per
 * StorageManager), so all accesses to the tracked page ranges are synchronized internally.
 */
class OptimisticAllocator : public PageAllocator {
public:
    explicit OptimisticAllocator(PageManager& pageManager);

    PageRange allocatePageRange(common::page_idx_t numPages) override;

    void freePageRange(PageRange block) override;

    void rollback();
    void commit();
    // A rolled-back row can remain in shared storage to preserve another writer's offsets.
    // Its pages then belong to storage and must be reclaimed by checkpoint, not rollback.
    void retainPageRange(PageRange block);

private:
    PageManager& pageManager;
    std::mutex mtx;
    std::vector<PageRange> optimisticallyAllocatedPages;
};
} // namespace storage
} // namespace lbug

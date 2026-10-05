#pragma once

#include <memory>
#include <vector>

#include "storage/page_range.h"

namespace lbug::storage {
class PageAllocator;
struct SegmentState;

// Pages a CSR scan already started reading. A checkpoint that rewrites those
// pages queues the free here instead of releasing them, and skips in-place
// updates that would overwrite the same pages. The free runs when the last
// scan drops the pin.
class PageReclaimDeferral {
public:
    static std::shared_ptr<PageReclaimDeferral> create();

    void defer(PageRange range, PageAllocator& pageAllocator);
    void defer(const SegmentState& state, PageAllocator& pageAllocator);
    // Drop queued frees without releasing the pages. Used when a checkpoint
    // throws and the old pages are still the live ones.
    void discard();
    void commit() { committed = true; }

    ~PageReclaimDeferral();

    static PageReclaimDeferral*& current();

private:
    struct Free {
        PageRange range;
        PageAllocator* pageAllocator;
    };

    std::vector<Free> frees;
    bool committed = false;
};

class PageReclaimDeferralScope {
public:
    explicit PageReclaimDeferralScope(PageReclaimDeferral* deferral)
        : previous{PageReclaimDeferral::current()} {
        PageReclaimDeferral::current() = deferral;
    }
    ~PageReclaimDeferralScope() { PageReclaimDeferral::current() = previous; }

    PageReclaimDeferralScope(const PageReclaimDeferralScope&) = delete;
    PageReclaimDeferralScope& operator=(const PageReclaimDeferralScope&) = delete;

private:
    PageReclaimDeferral* previous;
};

} // namespace lbug::storage

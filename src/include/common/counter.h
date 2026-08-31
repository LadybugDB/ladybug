#pragma once

#include <atomic>

#include "types/types.h"

namespace lbug {
namespace common {

class LimitCounter {
public:
    explicit LimitCounter(common::offset_t limitNumber) : limitNumber{limitNumber} {
        counter.store(0);
    }

    void increase(common::offset_t number) { counter.fetch_add(number); }

    bool exceedLimit() const { return counter.load() >= limitNumber; }

    // Re-arm the counter for another execution of a cached physical plan.
    void reset() { counter.store(0); }

private:
    common::offset_t limitNumber;
    std::atomic<common::offset_t> counter;
};

} // namespace common
} // namespace lbug

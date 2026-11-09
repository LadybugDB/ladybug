#pragma once

#include <atomic>
#include <chrono>
#include <string>

#include "common/assert.h"
#include "exception/exception.h"

namespace lbug {
namespace common {

class Timer {

public:
    void start() {
        finished = false;
        startTime.store(std::chrono::high_resolution_clock::now(), std::memory_order_release);
    }

    void stop() {
        stopTime = std::chrono::high_resolution_clock::now();
        finished = true;
    }

    double getDuration() const {
        if (finished) {
            auto duration = stopTime - startTime.load(std::memory_order_acquire);
            return (double)std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
        }
        throw Exception("Timer is still running.");
    }

    uint64_t getElapsedTimeInMS() const {
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = now - startTime.load(std::memory_order_acquire);
        auto count = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        DASSERT(count >= 0);
        return count;
    }

    double getElapsedTimeMS() const {
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = now - startTime.load(std::memory_order_acquire);
        return std::chrono::duration_cast<std::chrono::microseconds>(duration).count() / 1000.0;
    }

private:
    using TimePoint = std::chrono::time_point<std::chrono::high_resolution_clock>;
    static_assert(sizeof(std::atomic<TimePoint>) == sizeof(TimePoint) &&
                      alignof(std::atomic<TimePoint>) == alignof(TimePoint),
        "Timer is a member of ActiveQuery, whose layout is part of the extension ABI");

    // Atomic because a thread waiting on a query's task reads the query's timer while the thread
    // running the query restarts it for each statement (IMPORT DATABASE runs several).
    std::atomic<TimePoint> startTime;
    TimePoint stopTime;
    bool finished = false;
};

} // namespace common
} // namespace lbug

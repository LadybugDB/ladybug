#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "common/task_system/task_scheduler.h"
#include "gtest/gtest.h"
#include "main/client_context.h"
#include "main/connection.h"
#include "main/database.h"

using namespace lbug;

namespace {

void updatePeak(std::atomic<uint64_t>& peak, uint64_t now) {
    auto prev = peak.load();
    while (now > prev && !peak.compare_exchange_weak(prev, now)) {}
}

class CountingTask final : public common::Task {
public:
    CountingTask(std::atomic<uint64_t>& active, std::atomic<uint64_t>& peak,
        uint64_t maxNumThreads = 1)
        : Task{maxNumThreads}, active{active}, peak{peak} {}

    void run() override {
        threadID = std::this_thread::get_id();
        ++numRuns;
        updatePeak(peak, ++active);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        --active;
    }

    std::thread::id threadID;
    std::atomic<uint64_t> numRuns{0};

private:
    std::atomic<uint64_t>& active;
    std::atomic<uint64_t>& peak;
};

class BlockingTask final : public common::Task {
public:
    BlockingTask(std::atomic<uint64_t>& running, std::atomic<bool>& release)
        : Task{1}, running{running}, release{release} {}

    void run() override {
        ++running;
        // Spin so that every blocked caller gives its slot back at nearly the same moment.
        while (!release.load()) {}
    }

private:
    std::atomic<uint64_t>& running;
    std::atomic<bool>& release;
};

class NestingTask final : public common::Task {
public:
    NestingTask(main::ClientContext* context, std::shared_ptr<common::Task> child,
        bool launchNewWorkerThread = false)
        : Task{1}, context{context}, child{std::move(child)},
          launchNewWorkerThread{launchNewWorkerThread} {}

    void run() override {
        threadID = std::this_thread::get_id();
        common::TaskScheduler::Get(*context)->scheduleTaskAndWaitOrError(child, context,
            launchNewWorkerThread);
    }

    std::thread::id threadID;

private:
    main::ClientContext* context;
    std::shared_ptr<common::Task> child;
    bool launchNewWorkerThread;
};

void schedule(main::Connection& conn, const std::shared_ptr<common::Task>& task) {
    auto* context = conn.getClientContext();
    common::TaskScheduler::Get(*context)->scheduleTaskAndWaitOrError(task, context);
}

} // namespace

TEST(TaskSchedulerTest, OneThreadTaskRunsOnCallingThread) {
    main::Database db(":memory:", main::SystemConfig(-1u, 4));
    main::Connection conn(&db);
    std::atomic<uint64_t> active{0}, peak{0};
    auto task = std::make_shared<CountingTask>(active, peak);
    schedule(conn, task);
    EXPECT_EQ(task->threadID, std::this_thread::get_id());
}

TEST(TaskSchedulerTest, OneThreadTasksStayWithinThreadLimit) {
    constexpr uint64_t maxNumThreads = 2;
    constexpr uint64_t numCallers = 8;
    main::Database db(":memory:", main::SystemConfig(-1u, maxNumThreads));
    std::vector<std::unique_ptr<main::Connection>> conns;
    for (auto i = 0u; i < numCallers; i++) {
        conns.push_back(std::make_unique<main::Connection>(&db));
    }
    std::atomic<uint64_t> active{0}, peak{0};
    std::vector<std::thread> callers;
    for (auto& conn : conns) {
        callers.emplace_back([&, conn = conn.get()] {
            for (auto i = 0; i < 3; i++) {
                schedule(*conn, std::make_shared<CountingTask>(active, peak));
            }
        });
    }
    for (auto& caller : callers) {
        caller.join();
    }
    EXPECT_GE(peak.load(), 1u);
    EXPECT_LE(peak.load(), maxNumThreads);
}

// Callers that find every slot taken queue their tasks. When the inline callers finish, each
// freed slot must wake a worker, so the queued tasks run side by side.
TEST(TaskSchedulerTest, QueuedTasksRunInParallelOnceInlineSlotsFree) {
    constexpr uint64_t maxNumThreads = 4;
    main::Database db(":memory:", main::SystemConfig(-1u, maxNumThreads));
    std::vector<std::unique_ptr<main::Connection>> conns;
    for (auto i = 0u; i < 2 * maxNumThreads; i++) {
        conns.push_back(std::make_unique<main::Connection>(&db));
    }
    std::atomic<uint64_t> running{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> callers;
    for (auto i = 0u; i < maxNumThreads; i++) {
        callers.emplace_back([&, conn = conns[i].get()] {
            schedule(*conn, std::make_shared<BlockingTask>(running, release));
        });
    }
    while (running.load() < maxNumThreads) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::atomic<uint64_t> active{0}, peak{0};
    for (auto i = maxNumThreads; i < 2 * maxNumThreads; i++) {
        callers.emplace_back([&, conn = conns[i].get()] {
            schedule(*conn, std::make_shared<CountingTask>(active, peak));
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(peak.load(), 0u);
    release = true;
    for (auto& caller : callers) {
        caller.join();
    }
    EXPECT_EQ(peak.load(), maxNumThreads);
}

// With one slot held by the outer task, a nested one-thread task can only finish by reusing it.
TEST(TaskSchedulerTest, NestedOneThreadTaskReusesCallerSlot) {
    main::Database db(":memory:", main::SystemConfig(-1u, 1));
    main::Connection conn(&db);
    std::atomic<uint64_t> active{0}, peak{0};
    auto child = std::make_shared<CountingTask>(active, peak);
    auto outer = std::make_shared<NestingTask>(conn.getClientContext(), child);
    schedule(conn, outer);
    EXPECT_EQ(outer->threadID, std::this_thread::get_id());
    EXPECT_EQ(child->threadID, std::this_thread::get_id());
}

// A launchNewWorkerThread caller that holds the only slot blocks until its task completes, so a
// nested one-thread task can only finish on the new thread by taking over that slot.
TEST(TaskSchedulerTest, NewWorkerThreadTakesOverCallerSlot) {
    main::Database db(":memory:", main::SystemConfig(-1u, 1));
    main::Connection conn(&db);
    std::atomic<uint64_t> active{0}, peak{0};
    auto child = std::make_shared<CountingTask>(active, peak);
    auto inner = std::make_shared<NestingTask>(conn.getClientContext(), child);
    auto outer = std::make_shared<NestingTask>(conn.getClientContext(), inner,
        true /* launchNewWorkerThread */);
    schedule(conn, outer);
    EXPECT_EQ(outer->threadID, std::this_thread::get_id());
    EXPECT_NE(inner->threadID, std::this_thread::get_id());
    EXPECT_EQ(child->threadID, inner->threadID);
}

// A launchNewWorkerThread caller without a slot gives the new thread none, so a nested one-thread
// task waits while another caller holds the only slot.
TEST(TaskSchedulerTest, NewWorkerThreadOfSlotlessCallerHoldsNoSlot) {
    main::Database db(":memory:", main::SystemConfig(-1u, 1));
    main::Connection blockerConn(&db);
    main::Connection conn(&db);
    std::atomic<uint64_t> running{0};
    std::atomic<bool> release{false};
    std::thread blocker{
        [&] { schedule(blockerConn, std::make_shared<BlockingTask>(running, release)); }};
    while (running.load() < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::atomic<uint64_t> active{0}, peak{0};
    auto child = std::make_shared<CountingTask>(active, peak);
    auto inner = std::make_shared<NestingTask>(conn.getClientContext(), child);
    std::thread launcher{[&] {
        auto* context = conn.getClientContext();
        common::TaskScheduler::Get(*context)->scheduleTaskAndWaitOrError(inner, context,
            true /* launchNewWorkerThread */);
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(peak.load(), 0u);
    release = true;
    blocker.join();
    launcher.join();
    EXPECT_EQ(peak.load(), 1u);
}

// The only slot is held by the outer task, so no worker can join the nested task: the caller
// has to run it.
TEST(TaskSchedulerTest, NestedTaskRunsOnCallerWhenNoSlotIsFree) {
    main::Database db(":memory:", main::SystemConfig(-1u, 1));
    main::Connection conn(&db);
    std::atomic<uint64_t> active{0}, peak{0};
    auto child = std::make_shared<CountingTask>(active, peak, 2 /* maxNumThreads */);
    auto outer = std::make_shared<NestingTask>(conn.getClientContext(), child);
    schedule(conn, outer);
    EXPECT_EQ(child->threadID, outer->threadID);
}

#ifndef __SINGLE_THREADED__
// With a timeout the outer task runs on the only worker while the caller watches the clock. The
// nested task is not inlined up front, so the worker has to join it after queuing it.
TEST(TaskSchedulerTest, NestedTaskWithTimeoutRunsOnWorker) {
    main::Database db(":memory:", main::SystemConfig(-1u, 1));
    main::Connection conn(&db);
    conn.setQueryTimeOut(60000);
    conn.getClientContext()->startTimer();
    std::atomic<uint64_t> active{0}, peak{0};
    auto child = std::make_shared<CountingTask>(active, peak);
    auto outer = std::make_shared<NestingTask>(conn.getClientContext(), child);
    schedule(conn, outer);
    EXPECT_NE(outer->threadID, std::this_thread::get_id());
    EXPECT_EQ(child->threadID, outer->threadID);
}

// A launched thread stands in for the blocked caller, so the caller must not work on the task
// as well: that would put two threads on one slot.
TEST(TaskSchedulerTest, LaunchedThreadStandsInForCaller) {
    main::Database db(":memory:", main::SystemConfig(-1u, 1));
    main::Connection conn(&db);
    std::atomic<uint64_t> active{0}, peak{0};
    auto child = std::make_shared<CountingTask>(active, peak, 2 /* maxNumThreads */);
    auto outer = std::make_shared<NestingTask>(conn.getClientContext(), child,
        true /* launchNewWorkerThread */);
    schedule(conn, outer);
    EXPECT_EQ(child->numRuns.load(), 1u);
    EXPECT_NE(child->threadID, outer->threadID);
}

// A launched thread that took over the caller's only slot has to run the nested task itself.
TEST(TaskSchedulerTest, NestedTaskRunsOnLaunchedThread) {
    main::Database db(":memory:", main::SystemConfig(-1u, 1));
    main::Connection conn(&db);
    std::atomic<uint64_t> active{0}, peak{0};
    auto child = std::make_shared<CountingTask>(active, peak, 2 /* maxNumThreads */);
    auto inner = std::make_shared<NestingTask>(conn.getClientContext(), child);
    auto outer = std::make_shared<NestingTask>(conn.getClientContext(), inner,
        true /* launchNewWorkerThread */);
    schedule(conn, outer);
    EXPECT_NE(inner->threadID, std::this_thread::get_id());
    EXPECT_EQ(child->threadID, inner->threadID);
}
#endif

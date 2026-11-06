#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "common/profiler.h"
#include "gtest/gtest.h"
#include "main/connection.h"
#include "main/database.h"
#include "processor/execution_context.h"
#include "processor/operator/order_by/key_block_merger.h"
#include "processor/operator/order_by/order_by_merge.h"
#include "processor/processor_task.h"

using namespace lbug;

namespace {

constexpr auto SCAN_T = "MATCH (t:T) RETURN record_thread(t.id)";

std::mutex rowThreadsMtx;
std::set<std::thread::id> rowThreads;

int64_t recordThread(int64_t x) {
    std::lock_guard lck{rowThreadsMtx};
    rowThreads.insert(std::this_thread::get_id());
    return x;
}

class SingleMorselTaskTest : public ::testing::Test {
protected:
    void SetUp() override {
        rowThreads.clear();
        conn.createScalarFunction("record_thread", &recordThread);
        ASSERT_TRUE(conn.query("CREATE NODE TABLE T (id INT64, PRIMARY KEY (id))")->isSuccess());
    }

    void insertRows(uint64_t numRows) {
        auto result = conn.query(
            "UNWIND range(0, " + std::to_string(numRows - 1) + ") AS i CREATE (:T {id: i})");
        ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    }

    // Threads that evaluated the rows of `query`, which must return `numRows` rows.
    std::set<std::thread::id> getRowThreads(const std::string& query, uint64_t numRows) {
        rowThreads.clear();
        auto result = conn.query(query);
        EXPECT_TRUE(result->isSuccess()) << result->getErrorMessage();
        EXPECT_EQ(result->getNumTuples(), numRows);
        return rowThreads;
    }

    main::Database db{":memory:", main::SystemConfig(-1u /* bufferPoolSize */, 2 /* threads */)};
    main::Connection conn{&db};
};

TEST_F(SingleMorselTaskTest, OneMorselScanRunsOnCallingThread) {
    insertRows(50);
    EXPECT_EQ(getRowThreads(SCAN_T, 50), std::set{std::this_thread::get_id()});
}

// A single-threaded build runs every task on the calling thread.
#ifndef __SINGLE_THREADED__

// Registers threads on the task until it refuses one; stops at two, the pool size above.
uint64_t numThreadsAccepted(common::Task& task) {
    uint64_t numThreads = 0;
    while (numThreads < 2 && task.registerThread()) {
        numThreads++;
    }
    return numThreads;
}

TEST_F(SingleMorselTaskTest, MultiMorselScanRunsOnWorkers) {
    // One node group split into 2048-row morsels for the two threads: three morsels.
    insertRows(5000);
    auto threads = getRowThreads(SCAN_T, 5000);
    EXPECT_FALSE(threads.empty());
    EXPECT_FALSE(threads.contains(std::this_thread::get_id()));
}

TEST_F(SingleMorselTaskTest, UncommittedNodeGroupCountsAsMorsel) {
    insertRows(50);
    ASSERT_TRUE(conn.query("BEGIN TRANSACTION")->isSuccess());
    ASSERT_TRUE(conn.query("CREATE (:T {id: 50})")->isSuccess());
    auto threads = getRowThreads(SCAN_T, 51);
    EXPECT_FALSE(threads.empty());
    EXPECT_FALSE(threads.contains(std::this_thread::get_id()));
    ASSERT_TRUE(conn.query("ROLLBACK")->isSuccess());
}

TEST_F(SingleMorselTaskTest, OneMorselPerTableScanRunsOnWorkers) {
    insertRows(50);
    ASSERT_TRUE(conn.query("CREATE NODE TABLE U (id INT64, PRIMARY KEY (id))")->isSuccess());
    ASSERT_TRUE(conn.query("UNWIND range(0, 49) AS i CREATE (:U {id: i})")->isSuccess());
    auto threads = getRowThreads("MATCH (n) RETURN record_thread(n.id)", 100);
    EXPECT_FALSE(threads.empty());
    EXPECT_FALSE(threads.contains(std::this_thread::get_id()));
}

TEST_F(SingleMorselTaskTest, MergeOfOneSortedRunRunsOnOneThread) {
    auto sortState = std::make_shared<processor::SortSharedState>();
    processor::OrderByMerge merge{sortState,
        std::make_shared<processor::KeyBlockMergeTaskDispatcher>(), 0 /* id */,
        OPPrintInfo::EmptyInfo()};
    common::Profiler profiler;
    processor::ExecutionContext context{&profiler, conn.getClientContext(), 0 /* queryID */};
    // Merge tasks only read how many sorted runs there are, so empty runs stand in for them.
    sortState->appendLocalSortedKeyBlock(nullptr);
    processor::ProcessorTask oneRun{&merge, &context};
    oneRun.narrowToSingleMorselSource();
    EXPECT_EQ(numThreadsAccepted(oneRun), 1u);
    sortState->appendLocalSortedKeyBlock(nullptr);
    processor::ProcessorTask twoRuns{&merge, &context};
    twoRuns.narrowToSingleMorselSource();
    EXPECT_EQ(numThreadsAccepted(twoRuns), 2u);
}

#endif

} // namespace

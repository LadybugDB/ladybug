#pragma once

#include <mutex>
#include <thread>
#include <unordered_map>

#include "common/task_system/task_scheduler.h"

namespace lbug {
namespace main {
class QueryResult;
}
namespace storage {
class MemoryManager;
}
namespace processor {
class FactorizedTable;
class PhysicalPlan;
class PhysicalOperator;
class ResultSet;
struct ResultSetDescriptor;

// Per-database pool of reusable ResultSets, one slot per executing thread. A cached ResultSet
// owns ValueVectors whose buffers belong to this database's MemoryManager, so the pool must be
// owned by (and destroyed with) the database rather than living in a process-wide thread_local:
// a thread_local slot outlives Database::~Database and later releases (or worse, reuses) buffers
// through a dangling MemoryManager pointer when the next database runs a query on that thread.
class ResultSetPool {
public:
    // Returns the ResultSet cached for the calling thread when its descriptor id matches,
    // otherwise allocates a fresh one and caches it. The caller must keep the returned
    // shared_ptr for as long as it uses the ResultSet: a nested query on the same thread may
    // replace the slot, and the pool's own reference is dropped when this database closes.
    std::shared_ptr<ResultSet> getOrCreate(ResultSetDescriptor* descriptor,
        storage::MemoryManager* memoryManager);

private:
    struct Entry {
        uint64_t descriptorID = UINT64_MAX;
        std::shared_ptr<ResultSet> resultSet;
    };
    std::mutex mtx;
    std::unordered_map<std::thread::id, Entry> entries;
};

class QueryProcessor {

public:
#if defined(__APPLE__)
    explicit QueryProcessor(uint64_t numThreads, uint32_t threadQos);
#else
    explicit QueryProcessor(uint64_t numThreads);
#endif

    common::TaskScheduler* getTaskScheduler() { return taskScheduler.get(); }
    ResultSetPool& getResultSetPool() { return resultSetPool; }

    std::unique_ptr<main::QueryResult> execute(PhysicalPlan* physicalPlan,
        ExecutionContext* context);

private:
    void decomposePlanIntoTask(PhysicalOperator* op, common::Task* task, ExecutionContext* context);

    void initTask(common::Task* task);

private:
    // Declared before the scheduler so worker threads are joined before the pool is destroyed.
    // The pool never outlives the buffers it caches because Database destroys its QueryProcessor
    // before its MemoryManager.
    ResultSetPool resultSetPool;
    std::unique_ptr<common::TaskScheduler> taskScheduler;
};

} // namespace processor
} // namespace lbug

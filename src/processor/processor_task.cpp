#include "processor/processor_task.h"

#include "common/task_system/progress_bar.h"
#include "main/client_context.h"
#include "main/database.h"
#include "main/settings.h"
#include "processor/execution_context.h"
#include "processor/processor.h"
#include "processor/result/result_set.h"
#include "storage/buffer_manager/memory_manager.h"

using namespace lbug::common;

namespace lbug {
namespace processor {

ProcessorTask::ProcessorTask(Sink* sink, ExecutionContext* executionContext)
    : Task{executionContext->clientContext->getCurrentSetting(main::ThreadsSetting::name)
               .getValue<uint64_t>()},
      sharedStateInitialized{false}, sink{sink}, executionContext{executionContext} {}

void ProcessorTask::run() {
    // We need the lock when cloning because multiple threads can be accessing to clone,
    // which is not thread safe
    lock_t lck{taskMtx};
    if (!sharedStateInitialized) {
        sink->initGlobalState(executionContext);
        sharedStateInitialized = true;
    }
    auto taskRoot = sink->copy();
    lck.unlock();
    // Reuse the DataChunk / ValueVector / value-buffer allocations across
    // executions of the same prepared statement. The old code allocated a
    // fresh ResultSet per ProcessorTask::run() call; for a loop like
    //     for i in range(n): conn.execute("RETURN $i", {"i": i})
    // that's a 16KB+ calloc on every iteration.
    //
    // The reusable ResultSet is cached per executing thread in the database-owned
    // ResultSetPool (see QueryProcessor), never in a process-wide thread_local: the cached
    // vectors' buffers belong to this database's MemoryManager, and a thread_local slot would
    // outlive Database::~Database and free (or reuse) them through a dangling MemoryManager the
    // next time any database runs a query on this thread. Holding the shared_ptr here keeps the
    // ResultSet alive even if a nested query on this thread replaces the slot.
    ResultSet* resultSetPtr = nullptr;
    std::unique_ptr<ResultSet> ownedResultSet;
    std::shared_ptr<ResultSet> pooledResultSet;
    auto* clientContext = executionContext->clientContext;
    if (auto* desc = sink->getDescriptor()) {
        pooledResultSet =
            clientContext->getDatabase()->getQueryProcessor()->getResultSetPool().getOrCreate(desc,
                storage::MemoryManager::Get(*clientContext));
        resultSetPtr = pooledResultSet.get();
    } else {
        // No descriptor (e.g. OrderByMerge): fall back to per-call allocation.
        ownedResultSet =
            sink->getResultSet(storage::MemoryManager::Get(*executionContext->clientContext));
        resultSetPtr = ownedResultSet.get();
    }
    taskRoot->ptrCast<Sink>()->execute(resultSetPtr, executionContext);
}

void ProcessorTask::finalize() {
    ProgressBar::Get(*executionContext->clientContext)->finishPipeline(executionContext->queryID);
    sink->finalize(executionContext);
}

bool ProcessorTask::terminate() {
    return sink->terminate();
}

} // namespace processor
} // namespace lbug

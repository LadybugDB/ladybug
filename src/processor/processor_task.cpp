#include "processor/processor_task.h"

#include "common/task_system/progress_bar.h"
#include "main/client_context.h"
#include "main/settings.h"
#include "processor/execution_context.h"
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
    // We instead keep a thread-local shared_ptr<ResultSet> whose descriptor
    // matches this sink's, so each thread of a multi-threaded task gets
    // its own allocation-free slot. The thread owns the ResultSet outright
    // (no aliasing), so the lifetime is straightforward: dropped when the
    // thread exits, or when a different prepared statement runs on this
    // thread and the descriptor pointer no longer matches.
    ResultSet* resultSetPtr = nullptr;
    std::unique_ptr<ResultSet> ownedResultSet;
    if (auto* desc = sink->getDescriptor()) {
        thread_local uint64_t cachedDescID = UINT64_MAX;
        thread_local std::shared_ptr<processor::ResultSet> cachedResultSet;
        if (cachedDescID == desc->id && cachedResultSet) {
            // Same prepared statement on this thread: reuse the allocation.
            cachedResultSet->resetForReuse();
            resultSetPtr = cachedResultSet.get();
        } else {
            // First time on this thread, or a different prepared statement:
            // allocate fresh. Owning shared_ptr; lifetime ends with the
            // thread (or when the descriptor changes).
            cachedResultSet = std::make_shared<processor::ResultSet>(desc,
                storage::MemoryManager::Get(*executionContext->clientContext));
            cachedDescID = desc->id;
            resultSetPtr = cachedResultSet.get();
        }
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

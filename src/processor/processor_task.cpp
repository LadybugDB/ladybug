#include "processor/processor_task.h"

#include "common/task_system/progress_bar.h"
#include "main/client_context.h"
#include "main/settings.h"
#include "processor/execution_context.h"
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
    // Pick the ResultSet to hand to the cloned sink. We prefer the cached
    // one from the ExecutionContext (set up by the cached physical-plan
    // path) to skip per-execution DataChunk/ValueVector allocation, but
    // only when this task is single-threaded: a shared ResultSet would
    // race between worker threads writing to the same ValueVectors
    // concurrently. The old code created a fresh ResultSet per call, so
    // each thread had its own. We restore that behaviour for multi-
    // threaded tasks; the single-threaded path (the hot one for trivial
    // queries) keeps the allocation savings.
    ResultSet* resultSetPtr = nullptr;
    std::unique_ptr<ResultSet> ownedResultSet;
    if (maxNumThreads == 1) {
        resultSetPtr = executionContext->sharedResultSet;
    }
    if (resultSetPtr == nullptr) {
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

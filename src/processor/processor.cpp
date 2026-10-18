#include "processor/processor.h"

#include "common/task_system/progress_bar.h"
#include "main/client_context.h"
#include "main/query_result.h"
#include "processor/operator/sink.h"
#include "processor/physical_plan.h"
#include "processor/processor_task.h"
#include "processor/result/result_set.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace processor {

std::shared_ptr<ResultSet> ResultSetPool::getOrCreate(ResultSetDescriptor* descriptor,
    MemoryManager* memoryManager) {
    const auto threadID = std::this_thread::get_id();
    std::shared_ptr<ResultSet> reusable;
    {
        std::lock_guard lock{mtx};
        auto& entry = entries[threadID];
        if (entry.resultSet && entry.descriptorID == descriptor->id) {
            reusable = entry.resultSet;
        }
    }
    if (reusable) {
        // Same prepared statement on this thread: reuse the allocation. Only this thread touches
        // its slot's ResultSet, so the reset happens after the lock is released.
        reusable->resetForReuse();
        return reusable;
    }
    // First time on this thread, or a different prepared statement: allocate fresh outside the
    // lock, then swap it in and release the previous ResultSet after unlocking.
    auto resultSet = std::make_shared<ResultSet>(descriptor, memoryManager);
    std::shared_ptr<ResultSet> previous;
    {
        std::lock_guard lock{mtx};
        auto& entry = entries[threadID];
        previous = std::move(entry.resultSet);
        entry.resultSet = resultSet;
        entry.descriptorID = descriptor->id;
    }
    return resultSet;
}

#if defined(__APPLE__)
QueryProcessor::QueryProcessor(uint64_t numThreads, uint32_t threadQos) {
    taskScheduler = std::make_unique<TaskScheduler>(numThreads, threadQos);
}
#else
QueryProcessor::QueryProcessor(uint64_t numThreads) {
    taskScheduler = std::make_unique<TaskScheduler>(numThreads);
}
#endif

std::unique_ptr<main::QueryResult> QueryProcessor::execute(PhysicalPlan* physicalPlan,
    ExecutionContext* context) {
    context->clientContext->registerQueryStart();
    struct Guard {
        main::ClientContext* ctx;
        ~Guard() { ctx->registerQueryEnd(); }
    } guard{context->clientContext};
    auto lastOperator = physicalPlan->lastOperator.get();
    // The root pipeline(task) consists of operators and its prevOperator only, because we
    // expect to have linear plans. For binary operators, e.g., HashJoin, we  keep probe and its
    // prevOperator in the same pipeline, and decompose build and its prevOperator into another
    // one.
    auto sink = lastOperator->ptrCast<Sink>();
    auto task = std::make_shared<ProcessorTask>(sink, context);
    for (auto i = (int64_t)sink->getNumChildren() - 1; i >= 0; --i) {
        decomposePlanIntoTask(sink->getChild(i), task.get(), context);
    }
    initTask(task.get());
    auto progressBar = ProgressBar::Get(*context->clientContext);
    progressBar->startProgress(context->queryID);
    taskScheduler->scheduleTaskAndWaitOrError(task, context);
    progressBar->endProgress(context->queryID);
    return sink->getQueryResult();
}

void QueryProcessor::decomposePlanIntoTask(PhysicalOperator* op, Task* task,
    ExecutionContext* context) {
    if (op->isSource()) {
        ProgressBar::Get(*context->clientContext)->addPipeline();
    }
    if (op->isSink()) {
        auto childTask = std::make_unique<ProcessorTask>(dynamic_cast_checked<Sink*>(op), context);
        for (auto i = (int64_t)op->getNumChildren() - 1; i >= 0; --i) {
            decomposePlanIntoTask(op->getChild(i), childTask.get(), context);
        }
        task->addChildTask(std::move(childTask));
    } else {
        // Schedule the right most side (e.g., build side of the hash join) first.
        for (auto i = (int64_t)op->getNumChildren() - 1; i >= 0; --i) {
            decomposePlanIntoTask(op->getChild(i), task, context);
        }
    }
}

void QueryProcessor::initTask(Task* task) {
    auto processorTask = dynamic_cast_checked<ProcessorTask*>(task);
    PhysicalOperator* op = processorTask->sink;
    while (!op->isSource()) {
        if (!op->isParallel()) {
            task->setSingleThreadedTask();
        }
        op = op->getChild(0);
    }
    if (!op->isParallel()) {
        task->setSingleThreadedTask();
    }
    for (auto& child : task->children) {
        initTask(child.get());
    }
}

} // namespace processor
} // namespace lbug

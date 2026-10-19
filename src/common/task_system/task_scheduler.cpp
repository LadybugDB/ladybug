#include "common/task_system/task_scheduler.h"

#include "main/client_context.h"
#include "main/database.h"
#include "processor/processor.h"

#if defined(__APPLE__)
#include <pthread.h>

#include <pthread/qos.h>
#endif

namespace lbug {
namespace common {

#ifndef __SINGLE_THREADED__

namespace {

// The scheduler whose slot this thread holds while it runs a task: set for a worker's whole life
// (it runs tasks only while counted), and on a caller while it runs a task inline. A nested
// inline run on such a thread reuses its slot.
// Never outlives the scheduler: workers and launchNewWorkerThread threads are joined before
// it is destroyed, and SlotGuard restores a caller's previous value.
thread_local const TaskScheduler* busySlotOwner = nullptr;

} // namespace

#if defined(__APPLE__)
TaskScheduler::TaskScheduler(uint64_t numWorkerThreads, uint32_t threadQos)
#else
TaskScheduler::TaskScheduler(uint64_t numWorkerThreads)
#endif
    : stopWorkerThreads{false}, nextScheduledTaskID{0}, maxNumBusyThreads{numWorkerThreads},
      numBusyThreads{0} {
#if defined(__APPLE__)
    this->threadQos = threadQos;
#endif
    for (auto n = 0u; n < numWorkerThreads; ++n) {
        workerThreads.emplace_back([&] { runWorkerThread(); });
    }
}

TaskScheduler::~TaskScheduler() {
    lock_t lck{taskSchedulerMtx};
    stopWorkerThreads = true;
    lck.unlock();
    cv.notify_all();
    for (auto& thread : workerThreads) {
        thread.join();
    }
}

void TaskScheduler::scheduleTaskAndWaitOrError(const std::shared_ptr<Task>& task,
    processor::ExecutionContext* context, bool launchNewWorkerThread, TaskBeforeRunFn beforeRun) {
    scheduleTaskAndWaitOrError(task, context->clientContext, launchNewWorkerThread, beforeRun);
}

void TaskScheduler::scheduleTaskAndWaitOrError(const std::shared_ptr<Task>& task,
    main::ClientContext* clientContext, bool launchNewWorkerThread, TaskBeforeRunFn beforeRun) {
    for (auto& dependency : task->children) {
        scheduleTaskAndWaitOrError(dependency, clientContext, false, beforeRun);
        if (dependency->terminate()) {
            return;
        }
    }
    if (beforeRun != nullptr) {
        beforeRun(task.get());
    }
    // A one-thread task runs on the caller so the pool stays asleep. A timeout still uses
    // the pool: the waiting caller is what notices the clock and interrupts the worker.
    if (!launchNewWorkerThread && task->maxNumThreads == 1 &&
        !(clientContext != nullptr && clientContext->hasTimeout()) &&
        runOnCallingThread(task.get())) {
        if (task->hasException()) {
            std::rethrow_exception(task->getExceptionPtr());
        }
        return;
    }
    std::thread newWorkerThread;
    if (launchNewWorkerThread) {
        // Note that newWorkerThread is not executing yet. However, we still call
        // task->registerThread() function because the call in the next line will guarantee
        // that the thread starts working on it. registerThread() function only increases the
        // numThreadsRegistered field of the task, tt does not keep track of the thread ids or
        // anything specific to the thread.
        task->registerThread();
        // The caller blocks until the task completes, so the new thread takes over the caller's
        // slot if it holds one (a GDS call runs inside a task) and holds none otherwise.
        newWorkerThread = std::thread([taskPtr = task.get(), owner = busySlotOwner] {
            busySlotOwner = owner;
            runTask(taskPtr);
        });
    }
    auto scheduledTask = pushTaskIntoQueue(task);
    cv.notify_all();
    std::unique_lock<std::mutex> taskLck{task->taskMtx, std::defer_lock};
    while (true) {
        taskLck.lock();
        bool timedWait = false;
        auto timeout = 0u;
        if (task->isCompletedNoLock()) {
            // Note: we do not remove completed tasks from the queue in this function. They will be
            // removed by the worker threads when they traverse down the queue for a task to work on
            // (see getTaskAndRegister()).
            taskLck.unlock();
            break;
        }
        if (clientContext->hasTimeout()) {
            timeout = clientContext->getTimeoutRemainingInMS();
            if (timeout == 0) {
                clientContext->interrupt();
            } else {
                timedWait = true;
            }
        } else if (task->hasExceptionNoLock()) {
            // Interrupt tasks that errored, so other threads can stop working on them early.
            clientContext->interrupt();
        }
        if (timedWait) {
            task->cv.wait_for(taskLck, std::chrono::milliseconds(timeout));
        } else {
            task->cv.wait(taskLck);
        }
        taskLck.unlock();
    }
    if (launchNewWorkerThread) {
        newWorkerThread.join();
    }
    if (task->hasException()) {
        removeErroringTask(scheduledTask->ID);
        std::rethrow_exception(task->getExceptionPtr());
    }
}

bool TaskScheduler::runOnCallingThread(Task* task) {
    if (busySlotOwner == this) {
        if (!task->registerThread()) {
            return false;
        }
        runTask(task);
        return true;
    }
    {
        lock_t lck{taskSchedulerMtx};
        if (numBusyThreads == maxNumBusyThreads || !task->registerThread()) {
            return false;
        }
        numBusyThreads++;
    }
    // Gives the slot back even if runTask throws.
    struct SlotGuard {
        TaskScheduler* scheduler;
        const TaskScheduler* previousOwner;
        ~SlotGuard() {
            busySlotOwner = previousOwner;
            lock_t lck{scheduler->taskSchedulerMtx};
            scheduler->numBusyThreads--;
            // A queued task may be waiting for this slot rather than for a worker.
            if (!scheduler->taskQueue.empty()) {
                scheduler->cv.notify_one();
            }
        }
    } guard{this, busySlotOwner};
    busySlotOwner = this;
    // Writes made here reach a dependent task: it runs on this thread, or this thread queues it
    // under taskSchedulerMtx. Writes of a child run by workers arrive through its taskMtx.
    runTask(task);
    return true;
}

void TaskScheduler::runWorkerThread() {
    busySlotOwner = this;
#if defined(__APPLE__)
    qos_class_t qosClass = (qos_class_t)threadQos;
    if (qosClass != QOS_CLASS_DEFAULT && qosClass != QOS_CLASS_UNSPECIFIED) {
        auto pthreadQosStatus = pthread_set_qos_class_self_np(qosClass, 0);
        UNUSED(pthreadQosStatus);
    }
#endif
    std::unique_lock<std::mutex> lck{taskSchedulerMtx, std::defer_lock};
    std::exception_ptr exceptionPtr = nullptr;
    std::shared_ptr<ScheduledTask> scheduledTask = nullptr;
    while (true) {
        // Warning: Threads acquire a global lock (using taskSchedulerMutex) right before
        // deregistering themselves from a task (and they immediately register themselves for
        // another task without releasing the lock). This acquire-right-before-deregistering ensures
        // that all writes that were done by threads in Task_j happen before a Task_{j+1} which
        // depends on Task_j can start. That's because before Task_{j+1} can start, each thread T_i
        // working on Task_j will need to deregister itself using the global lock. Therefore, by the
        // time any thread gets to start on Task_{j+1}, all writes made to Task_j by T_i will become
        // globally visible because T_i grabbed the global lock before deregistering (and without
        // T_i deregistering Task_{j+1} cannot start).
        lck.lock();
        if (scheduledTask != nullptr) {
            if (exceptionPtr != nullptr) {
                scheduledTask->task->setException(exceptionPtr);
                exceptionPtr = nullptr;
            }
            scheduledTask->task->deRegisterThreadAndFinalizeTask();
            scheduledTask = nullptr;
            numBusyThreads--;
        }
        cv.wait(lck, [&] {
            if (numBusyThreads < maxNumBusyThreads) {
                scheduledTask = getTaskAndRegister();
            }
            return scheduledTask != nullptr || stopWorkerThreads;
        });
        if (scheduledTask != nullptr) {
            numBusyThreads++;
        }
        lck.unlock();
        if (stopWorkerThreads) {
            return;
        }
        try {
            scheduledTask->task->run();
        } catch (std::exception& e) {
            exceptionPtr = std::current_exception();
        }
    }
}
#else
// Single-threaded version of TaskScheduler
TaskScheduler::TaskScheduler(uint64_t) : stopWorkerThreads{false}, nextScheduledTaskID{0} {}

TaskScheduler::~TaskScheduler() {
    stopWorkerThreads = true;
}

void TaskScheduler::scheduleTaskAndWaitOrError(const std::shared_ptr<Task>& task,
    processor::ExecutionContext* context, bool launchNewWorkerThread, TaskBeforeRunFn beforeRun) {
    scheduleTaskAndWaitOrError(task, context->clientContext, launchNewWorkerThread, beforeRun);
}

void TaskScheduler::scheduleTaskAndWaitOrError(const std::shared_ptr<Task>& task,
    main::ClientContext* clientContext, bool, TaskBeforeRunFn beforeRun) {
    for (auto& dependency : task->children) {
        scheduleTaskAndWaitOrError(dependency, clientContext, false, beforeRun);
        if (dependency->terminate()) {
            return;
        }
    }
    if (beforeRun != nullptr) {
        beforeRun(task.get());
    }
    task->registerThread();
    // runTask deregisters, so we don't need to deregister explicitly here
    runTask(task.get());
    if (task->hasException()) {
        removeErroringTask(task->ID);
        std::rethrow_exception(task->getExceptionPtr());
    }
}
#endif

std::shared_ptr<ScheduledTask> TaskScheduler::pushTaskIntoQueue(const std::shared_ptr<Task>& task) {
    lock_t lck{taskSchedulerMtx};
    auto scheduledTask = std::make_shared<ScheduledTask>(task, nextScheduledTaskID++);
    taskQueue.push_back(scheduledTask);
    return scheduledTask;
}

std::shared_ptr<ScheduledTask> TaskScheduler::getTaskAndRegister() {
    if (taskQueue.empty()) {
        return nullptr;
    }
    auto it = taskQueue.begin();
    while (it != taskQueue.end()) {
        auto task = (*it)->task;
        if (!task->registerThread()) {
            // If we cannot register for a thread it is because of three possibilities:
            // (i) maximum number of threads have registered for task and the task is completed
            // without an exception; or (ii) same as (i) but the task has not yet successfully
            // completed; or (iii) task has an exception; Only in (i) we remove the task from the
            // queue. For (ii) and (iii) we keep the task in queue. Recall erroring tasks need to be
            // manually removed.
            if (task->isCompletedSuccessfully()) { // option (i)
                it = taskQueue.erase(it);
            } else { // option (ii) or (iii): keep the task in the queue.
                ++it;
            }
        } else {
            return *it;
        }
    }
    return nullptr;
}

void TaskScheduler::removeErroringTask(uint64_t scheduledTaskID) {
    lock_t lck{taskSchedulerMtx};
    for (auto it = taskQueue.begin(); it != taskQueue.end(); ++it) {
        if (scheduledTaskID == (*it)->ID) {
            taskQueue.erase(it);
            return;
        }
    }
}

void TaskScheduler::runTask(Task* task) {
    try {
        task->run();
        task->deRegisterThreadAndFinalizeTask();
    } catch (std::exception& e) {
        task->setException(std::current_exception());
        task->deRegisterThreadAndFinalizeTask();
    }
}

TaskScheduler* TaskScheduler::Get(const main::ClientContext& context) {
    return context.getDatabase()->getQueryProcessor()->getTaskScheduler();
}

} // namespace common
} // namespace lbug

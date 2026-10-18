#pragma once

#include "common/task_system/task.h"
#include "processor/operator/sink.h"

namespace lbug {
namespace processor {

class ProcessorTask : public common::Task {
    friend class QueryProcessor;

public:
    ProcessorTask(Sink* sink, ExecutionContext* executionContext);

    void run() override;

    void finalize() override;

    bool terminate() override;

    // A READ_FTABLE source that already fits in one morsel has no work for a second thread.
    // Called after child tasks, so an accumulate scan sees the filled table.
    void narrowToSingleMorselSource();

private:
    bool sharedStateInitialized;
    Sink* sink;
    ExecutionContext* executionContext;
};

} // namespace processor
} // namespace lbug

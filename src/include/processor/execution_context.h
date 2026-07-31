#pragma once

#include "common/profiler.h"

namespace lbug {
namespace main {
class ClientContext;
}
namespace processor {
class ResultSet;

struct LBUG_API ExecutionContext {
    uint64_t queryID;
    common::Profiler* profiler;
    main::ClientContext* clientContext;
    // Optional pre-attached ResultSet (non-owning). When set, the processor
    // reuses this ResultSet instead of allocating a new one via
    // Sink::getResultSet(). Used by the cached physical-plan path to skip
    // per-execution DataChunk/ValueVector allocation. The pointee is owned
    // by the CachedPreparedStatement and outlives this ExecutionContext.
    ResultSet* sharedResultSet = nullptr;

    ExecutionContext(common::Profiler* profiler, main::ClientContext* clientContext,
        uint64_t queryID)
        : queryID{queryID}, profiler{profiler}, clientContext{clientContext} {}
};

} // namespace processor
} // namespace lbug

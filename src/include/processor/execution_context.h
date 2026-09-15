#pragma once

#include "common/profiler.h"

namespace lbug {
namespace main {
class ClientContext;
}
namespace processor {

// EXTENSION ABI: extensions ship once per minor version and must keep working with
// patch-release CLIs, while the inline constructor bakes member offsets into extension
// binaries. NEVER reorder/remove data members; ALWAYS append new members at the END (see #971).
struct LBUG_API ExecutionContext {
    uint64_t queryID;
    common::Profiler* profiler;
    main::ClientContext* clientContext;

    ExecutionContext(common::Profiler* profiler, main::ClientContext* clientContext,
        uint64_t queryID)
        : queryID{queryID}, profiler{profiler}, clientContext{clientContext} {}
};

} // namespace processor
} // namespace lbug

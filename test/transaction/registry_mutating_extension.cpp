#include "main/client_context.h"
#include "main/database_manager.h"

namespace {
constexpr auto DROP_GRAPH_NAME = "extension_drop_target";
}

extern "C" {
// The extension loads live on the standalone graph session before its record is replayed
// on the main database, so init must no-op when the graph is not loaded there.
#if defined(_WIN32)
#define INIT_EXPORT __declspec(dllexport)
#else
#define INIT_EXPORT __attribute__((visibility("default")))
#endif
INIT_EXPORT void init(lbug::main::ClientContext* context) {
    auto databaseManager = lbug::main::DatabaseManager::Get(*context);
    if (databaseManager->hasGraph(DROP_GRAPH_NAME)) {
        databaseManager->dropGraph(DROP_GRAPH_NAME, context);
    }
}

INIT_EXPORT const char* name() {
    return "registry_mutator";
}
}

#include "main/neo4j_extension.h"

#include "function/neo4j_migrate.h"
#include "main/client_context.h"

namespace lbug {
namespace neo4j_extension {

using namespace extension;

void Neo4jExtension::load(main::ClientContext* context) {
    auto& db = *context->getDatabase();
    ExtensionUtils::addStandaloneTableFunc<Neo4jMigrateFunction>(db);
}

} // namespace neo4j_extension
} // namespace lbug

#if defined(BUILD_DYNAMIC_LOAD)
extern "C" {
// Because we link against the static library on windows, we implicitly inherit LBUG_STATIC_DEFINE,
// which cancels out any exporting, so we can't use LBUG_API.
#if defined(_WIN32)
#define INIT_EXPORT __declspec(dllexport)
#else
#define INIT_EXPORT __attribute__((visibility("default")))
#endif
INIT_EXPORT void init(lbug::main::ClientContext* context) {
    lbug::neo4j_extension::Neo4jExtension::load(context);
}

INIT_EXPORT const char* name() {
    return lbug::neo4j_extension::Neo4jExtension::EXTENSION_NAME;
}
}
#endif

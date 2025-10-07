#pragma once

#include "function/function.h"

namespace lbug {
namespace neo4j_extension {

struct Neo4jMigrateFunction {
    static constexpr const char* name = "NEO4J_MIGRATE";

    static function::function_set getFunctionSet();
};

} // namespace neo4j_extension
} // namespace lbug

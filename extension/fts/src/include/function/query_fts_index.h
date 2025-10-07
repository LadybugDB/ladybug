#pragma once

#include "function/gds/gds.h"
#include "function/gds/gds_frontier.h"

namespace lbug {
namespace fts_extension {

struct QueryFTSFunction {
    static constexpr const char* name = "QUERY_FTS_INDEX";

    static function::function_set getFunctionSet();
};

} // namespace fts_extension
} // namespace lbug

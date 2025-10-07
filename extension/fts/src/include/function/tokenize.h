#pragma once

#include "function/function.h"

namespace lbug {
namespace fts_extension {

struct TokenizeFunction {
    static constexpr const char* name = "TOKENIZE";

    static function::function_set getFunctionSet();
};

} // namespace fts_extension
} // namespace lbug

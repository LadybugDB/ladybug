#pragma once

#include "function/function.h"

namespace lbug {
namespace json_extension {

struct CastToJsonFunction {
    static constexpr const char* name = "CAST_TO_JSON";

    static function::function_set getFunctionSet();
};

} // namespace json_extension
} // namespace lbug

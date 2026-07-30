#pragma once

#include "function/aggregate_function.h"

namespace lbug {
namespace function {

struct AggregateHistogramFunction {
    static constexpr const char* name = "HISTOGRAM";
    static constexpr const char* prettyName = "histogram";

    static function_set getFunctionSet();
};

} // namespace function
} // namespace lbug

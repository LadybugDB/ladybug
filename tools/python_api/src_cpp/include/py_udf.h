#pragma once

#include <string>

#include "common/types/types.h"
#include "function/function.h"
#include "pybind_include.h"

using lbug::common::LogicalTypeID;
using lbug::function::function_set;

namespace lbug {
namespace main {
class ClientContext;
} // namespace main
} // namespace lbug

class PyUDF {

public:
    static function_set toFunctionSet(const std::string& name, const py::function& udf,
        const py::list& paramTypes, const std::string& resultType, bool defaultNull,
        bool catchExceptions, lbug::main::ClientContext* context);
};

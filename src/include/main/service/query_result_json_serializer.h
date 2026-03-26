#pragma once

#include <string>

#include "main/query_result.h"

namespace lbug {
namespace main {

std::string queryResultToJson(QueryResult& result);

} // namespace main
} // namespace lbug

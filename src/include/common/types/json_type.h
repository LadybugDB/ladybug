#pragma once

#include "common/types/types.h"

namespace lbug {
namespace common {

struct JsonType {
    static constexpr char JSON_TYPE_NAME[] = "json";

    static LogicalType getJsonType();
    static bool isJson(const LogicalType& type);
};

} // namespace common
} // namespace lbug

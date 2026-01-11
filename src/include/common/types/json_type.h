#pragma once

#include "common/types/types.h"

namespace lbug {
namespace common {

struct LBUG_API JsonType {
    static constexpr char JSON_TYPE_NAME[] = "JSON";

    static LogicalType getJsonType();
    static bool isJson(const LogicalType& type);
};

} // namespace common
} // namespace lbug

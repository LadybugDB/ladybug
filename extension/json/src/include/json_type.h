#pragma once

#include "common/types/types.h"

namespace lbug {
namespace json_extension {

struct JsonType {
    static common::LogicalType getJsonType();
    static bool isJson(const common::LogicalType& type);
};

} // namespace json_extension
} // namespace lbug

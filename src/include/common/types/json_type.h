#pragma once

#include "common/types/types.h"

namespace lbug {
namespace common {

struct JsonType {
    static constexpr char JSON_TYPE_NAME[] = "JSON";

    static inline LogicalType getJsonType() { return LogicalType::JSON(); }

    static inline bool isJson(const LogicalType& type) {
        return type.getLogicalTypeID() == LogicalTypeID::JSON;
    }
};

} // namespace common
} // namespace lbug

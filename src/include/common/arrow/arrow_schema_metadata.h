#pragma once

#include <cstdint>
#include <optional>

#include "common/api.h"
#include "common/arrow/arrow.h"

namespace lbug {
namespace common {

struct ArrowDecimalTypeInfo {
    uint32_t precision;
    uint32_t scale;
};

struct ArrowLogicalTypeInfo {
    enum class Source : uint8_t {
        SNOWFLAKE,
        GENERIC_METADATA,
    };

    enum class Type : uint8_t {
        DECIMAL,
    };

    Source source;
    Type type;
    ArrowDecimalTypeInfo decimal;
};

LBUG_API std::optional<ArrowLogicalTypeInfo> tryGetArrowLogicalTypeInfo(const ArrowSchema* schema);

} // namespace common
} // namespace lbug

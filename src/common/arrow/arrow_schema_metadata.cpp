#include "common/arrow/arrow_schema_metadata.h"

#include "arrow_schema_metadata_internal.h"

namespace lbug {
namespace common {

std::optional<ArrowLogicalTypeInfo> tryGetArrowLogicalTypeInfo(const ArrowSchema* schema) {
    if (schema == nullptr || schema->format == nullptr || schema->metadata == nullptr) {
        return std::nullopt;
    }
    const auto metadata = readArrowMetadata(schema->metadata);
    if (auto snowflakeRawDataTypeInfo = tryParseSnowflakeRawDataTypeInfo(metadata);
        snowflakeRawDataTypeInfo.has_value()) {
        return snowflakeRawDataTypeInfo;
    }
    if (auto snowflakeTypeInfo = tryParseSnowflakeLogicalTypeInfo(schema, metadata);
        snowflakeTypeInfo.has_value()) {
        return snowflakeTypeInfo;
    }
    return tryParseGenericIntegerBackedDecimalMetadata(schema, metadata);
}

} // namespace common
} // namespace lbug

#include "arrow_schema_metadata_internal.h"

namespace lbug {
namespace common {

std::optional<ArrowLogicalTypeInfo> tryParseGenericIntegerBackedDecimalMetadata(
    const ArrowSchema* schema, const ArrowMetadataMap& metadata) {
    if (!isIntegralArrowStorageType(schema->format)) {
        return std::nullopt;
    }
    const auto logicalType = getMetadataValue(metadata, "logicaltype");
    if (!logicalType.has_value()) {
        return std::nullopt;
    }
    const auto normalized = toLower(*logicalType);
    if (normalized != "decimal" && normalized != "number" && normalized != "numeric") {
        return std::nullopt;
    }
    const auto precision = getMetadataValue(metadata, "precision");
    const auto scale = getMetadataValue(metadata, "scale");
    if (!precision.has_value() || !scale.has_value()) {
        return std::nullopt;
    }
    const auto parsedPrecision = tryParseUint32(*precision);
    const auto parsedScale = tryParseUint32(*scale);
    if (!parsedPrecision.has_value() || !parsedScale.has_value() ||
        !isValidDecimalParameters(*parsedPrecision, *parsedScale)) {
        return std::nullopt;
    }
    return ArrowLogicalTypeInfo{ArrowLogicalTypeInfo::Source::GENERIC_METADATA,
        ArrowLogicalTypeInfo::Type::DECIMAL, ArrowDecimalTypeInfo{*parsedPrecision, *parsedScale}};
}

} // namespace common
} // namespace lbug

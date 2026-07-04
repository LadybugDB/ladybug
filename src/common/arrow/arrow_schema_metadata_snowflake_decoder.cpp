#include "arrow_schema_metadata_internal.h"

namespace lbug {
namespace common {

namespace {

std::optional<ArrowDecimalTypeInfo> tryParseSnowflakeDecimalType(const std::string& rawDataType) {
    auto normalized = toLower(trim(rawDataType));
    const auto openParen = normalized.find('(');
    if (openParen == std::string::npos) {
        return std::nullopt;
    }
    const auto typeName = trim(normalized.substr(0, openParen));
    if (typeName != "number" && typeName != "numeric" && typeName != "decimal") {
        return std::nullopt;
    }
    const auto closeParen = normalized.find(')', openParen + 1);
    if (closeParen == std::string::npos) {
        return std::nullopt;
    }
    auto args = splitCommaSeparated(normalized.substr(openParen + 1, closeParen - openParen - 1));
    if (args.empty() || args.size() > 2) {
        return std::nullopt;
    }
    const auto precision = tryParseUint32(args[0]);
    if (!precision.has_value()) {
        return std::nullopt;
    }
    auto scale = std::optional<uint32_t>{0};
    if (args.size() == 2) {
        scale = tryParseUint32(args[1]);
    }
    if (!scale.has_value() || !isValidDecimalParameters(*precision, *scale)) {
        return std::nullopt;
    }
    return ArrowDecimalTypeInfo{*precision, *scale};
}

} // namespace

std::optional<ArrowLogicalTypeInfo> tryParseSnowflakeRawDataTypeInfo(
    const ArrowMetadataMap& metadata) {
    const auto rawDataType = getMetadataValue(metadata, "data_type");
    if (!rawDataType.has_value()) {
        return std::nullopt;
    }
    const auto decimalInfo = tryParseSnowflakeDecimalType(*rawDataType);
    if (!decimalInfo.has_value()) {
        return std::nullopt;
    }
    return ArrowLogicalTypeInfo{ArrowLogicalTypeInfo::Source::SNOWFLAKE,
        ArrowLogicalTypeInfo::Type::DECIMAL, *decimalInfo};
}

std::optional<ArrowLogicalTypeInfo> tryParseSnowflakeLogicalTypeInfo(const ArrowSchema* schema,
    const ArrowMetadataMap& metadata) {
    if (!isIntegralArrowStorageType(schema->format) &&
        !isFloatingArrowStorageType(schema->format)) {
        return std::nullopt;
    }
    const auto logicalType = getMetadataValue(metadata, "logicaltype");
    if (!logicalType.has_value()) {
        return std::nullopt;
    }
    const auto normalized = toLower(*logicalType);
    if (normalized != "fixed") {
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
    return ArrowLogicalTypeInfo{ArrowLogicalTypeInfo::Source::SNOWFLAKE,
        ArrowLogicalTypeInfo::Type::DECIMAL, ArrowDecimalTypeInfo{*parsedPrecision, *parsedScale}};
}

} // namespace common
} // namespace lbug

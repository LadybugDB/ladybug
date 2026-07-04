#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "common/arrow/arrow_schema_metadata.h"

namespace lbug {
namespace common {

using ArrowMetadataMap = std::map<std::string, std::string>;

bool isIntegralArrowStorageType(const char* arrowType);
bool isFloatingArrowStorageType(const char* arrowType);
std::string toLower(std::string value);
ArrowMetadataMap readArrowMetadata(const char* metadata);
std::string trim(std::string value);
std::vector<std::string> splitCommaSeparated(std::string value);
std::optional<std::string> getMetadataValue(const ArrowMetadataMap& metadata,
    const std::string& key);
std::optional<uint32_t> tryParseUint32(const std::string& value);
bool isValidDecimalParameters(uint32_t precision, uint32_t scale);

std::optional<ArrowLogicalTypeInfo> tryParseSnowflakeRawDataTypeInfo(
    const ArrowMetadataMap& metadata);
std::optional<ArrowLogicalTypeInfo> tryParseSnowflakeLogicalTypeInfo(const ArrowSchema* schema,
    const ArrowMetadataMap& metadata);
std::optional<ArrowLogicalTypeInfo> tryParseGenericIntegerBackedDecimalMetadata(
    const ArrowSchema* schema, const ArrowMetadataMap& metadata);

} // namespace common
} // namespace lbug

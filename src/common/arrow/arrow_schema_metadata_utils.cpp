#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

#include "arrow_schema_metadata_internal.h"
#include "common/constants.h"

namespace lbug {
namespace common {

bool isIntegralArrowStorageType(const char* arrowType) {
    switch (arrowType[0]) {
    case 'c':
    case 'C':
    case 's':
    case 'S':
    case 'i':
    case 'I':
    case 'l':
    case 'L':
        return true;
    default:
        return false;
    }
}

bool isFloatingArrowStorageType(const char* arrowType) {
    return arrowType[0] == 'f' || arrowType[0] == 'g';
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

ArrowMetadataMap readArrowMetadata(const char* metadata) {
    ArrowMetadataMap result;
    if (metadata == nullptr) {
        return result;
    }
    const auto* ptr = reinterpret_cast<const uint8_t*>(metadata);
    size_t bytesRead = 0;
    auto tryReadInt32 = [&](int32_t& out) {
        if (std::numeric_limits<size_t>::max() - bytesRead < sizeof(int32_t)) {
            return false;
        }
        memcpy(&out, ptr, sizeof(int32_t));
        ptr += sizeof(int32_t);
        bytesRead += sizeof(int32_t);
        return true;
    };
    auto tryReadString = [&](std::string& out) {
        int32_t len = 0;
        if (!tryReadInt32(len) || len < 0) {
            return false;
        }
        const auto lenSize = static_cast<size_t>(len);
        if (std::numeric_limits<size_t>::max() - bytesRead < lenSize) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(ptr), lenSize);
        ptr += lenSize;
        bytesRead += lenSize;
        return true;
    };

    int32_t numEntries = 0;
    if (!tryReadInt32(numEntries) || numEntries < 0) {
        return {};
    }
    for (auto i = 0; i < numEntries; ++i) {
        std::string key;
        std::string value;
        // ArrowSchema.metadata does not expose an outer byte length, so we can validate shape
        // (negative lengths, arithmetic overflow) but not fully bounds-check truncated buffers.
        if (!tryReadString(key) || !tryReadString(value)) {
            return {};
        }

        result.emplace(toLower(std::move(key)), std::move(value));
    }
    return result;
}

std::string trim(std::string value) {
    value.erase(value.begin(),
        std::find_if(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); }));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), [](unsigned char c) { return !std::isspace(c); })
            .base(),
        value.end());
    return value;
}

std::vector<std::string> splitCommaSeparated(std::string value) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(',', start);
        auto part =
            end == std::string::npos ? value.substr(start) : value.substr(start, end - start);
        result.push_back(trim(std::move(part)));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

std::optional<std::string> getMetadataValue(const ArrowMetadataMap& metadata,
    const std::string& key) {
    const auto entry = metadata.find(key);
    if (entry == metadata.end()) {
        return std::nullopt;
    }
    return entry->second;
}

std::optional<uint32_t> tryParseUint32(const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }
    uint32_t parsed = 0;
    for (auto c : value) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
        const auto digit = static_cast<uint32_t>(c - '0');
        if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        parsed = parsed * 10 + digit;
    }
    return parsed;
}

bool isValidDecimalParameters(uint32_t precision, uint32_t scale) {
    return precision > 0 && precision <= DECIMAL_PRECISION_LIMIT && scale <= precision;
}

} // namespace common
} // namespace lbug

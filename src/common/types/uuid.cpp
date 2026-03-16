#include "common/types/uuid.h"

#include "common/exception/conversion.h"
#include "common/random_engine.h"
#include "re2.h"

namespace lbug {
namespace common {

void uuid::byteToHex(char byteVal, char* buf, uint64_t& pos) {
    buf[pos++] = HEX_DIGITS[(byteVal >> 4) & 0xf];
    buf[pos++] = HEX_DIGITS[byteVal & 0xf];
}

unsigned char uuid::hex2Char(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return 0;
}

bool uuid::isHex(char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

bool uuid::fromString(std::string str, uuid& result) {
    if (str.empty()) {
        return false;
    }
    uint32_t numBrackets = 0;
    if (str.front() == '{') {
        numBrackets = 1;
    }
    // LCOV_EXCL_START
    if (numBrackets && str.back() != '}') {
        return false;
    }
    // LCOV_EXCL_STOP

    int128_t value(0, 0);

    uint32_t count = 0;
    for (auto i = numBrackets; i < str.size() - numBrackets; ++i) {
        if (str[i] == '-') {
            continue;
        }
        if (count >= 32 || !isHex(str[i])) {
            return false;
        }
        if (count >= 16) {
            value.low = (value.low << 4) | hex2Char(str[i]);
        } else {
            value.high = (value.high << 4) | hex2Char(str[i]);
        }
        count++;
    }
    // Flip the first bit to make `order by uuid` same as `order by uuid::varchar`
    value.high ^= (int64_t(1) << 63);
    result.value = value;

    return count == 32;
}

uuid uuid::fromString(std::string str) {
    uuid result;
    if (!fromString(str, result)) {
        throw ConversionException("Invalid UUID: " + str);
    }
    return result;
}

uuid uuid::fromCString(const char* str, uint64_t len) {
    return fromString(std::string(str, len));
}

void uuid::toString(int128_t input, char* buf) {
    // Flip back before convert to string
    int64_t high = input.high ^ (int64_t(1) << 63);
    uint64_t pos = 0;
    byteToHex(high >> 56 & 0xFF, buf, pos);
    byteToHex(high >> 48 & 0xFF, buf, pos);
    byteToHex(high >> 40 & 0xFF, buf, pos);
    byteToHex(high >> 32 & 0xFF, buf, pos);
    buf[pos++] = '-';
    byteToHex(high >> 24 & 0xFF, buf, pos);
    byteToHex(high >> 16 & 0xFF, buf, pos);
    buf[pos++] = '-';
    byteToHex(high >> 8 & 0xFF, buf, pos);
    byteToHex(high & 0xFF, buf, pos);
    buf[pos++] = '-';
    byteToHex(input.low >> 56 & 0xFF, buf, pos);
    byteToHex(input.low >> 48 & 0xFF, buf, pos);
    buf[pos++] = '-';
    byteToHex(input.low >> 40 & 0xFF, buf, pos);
    byteToHex(input.low >> 32 & 0xFF, buf, pos);
    byteToHex(input.low >> 24 & 0xFF, buf, pos);
    byteToHex(input.low >> 16 & 0xFF, buf, pos);
    byteToHex(input.low >> 8 & 0xFF, buf, pos);
    byteToHex(input.low & 0xFF, buf, pos);
}

std::string uuid::toString(int128_t input) {
    char buff[UUID_STRING_LENGTH];
    toString(input, buff);
    return std::string(buff, UUID_STRING_LENGTH);
}

std::string uuid::toString(uuid val) {
    return toString(val.value);
}

uuid uuid::generateRandomUUID(RandomEngine* engine) {
    uint8_t bytes[16];
    for (int i = 0; i < 16; i += 4) {
        *reinterpret_cast<uint32_t*>(bytes + i) = engine->nextRandomInteger();
    }
    // variant must be 10xxxxxx
    bytes[8] &= 0xBF;
    bytes[8] |= 0x80;
    // version must be 0100xxxx
    bytes[6] &= 0x4F;
    bytes[6] |= 0x40;

    int128_t result = 0;
    result.high = 0;
    result.high |= ((int64_t)bytes[0] << 56);
    result.high |= ((int64_t)bytes[1] << 48);
    result.high |= ((int64_t)bytes[2] << 40);
    result.high |= ((int64_t)bytes[3] << 32);
    result.high |= ((int64_t)bytes[4] << 24);
    result.high |= ((int64_t)bytes[5] << 16);
    result.high |= ((int64_t)bytes[6] << 8);
    result.high |= bytes[7];
    result.low = 0;
    result.low |= ((uint64_t)bytes[8] << 56);
    result.low |= ((uint64_t)bytes[9] << 48);
    result.low |= ((uint64_t)bytes[10] << 40);
    result.low |= ((uint64_t)bytes[11] << 32);
    result.low |= ((uint64_t)bytes[12] << 24);
    result.low |= ((uint64_t)bytes[13] << 16);
    result.low |= ((uint64_t)bytes[14] << 8);
    result.low |= bytes[15];
    return uuid{result};
}

const regex::RE2& uuid::regexPattern() {
    static regex::RE2 retval("(?i)[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}");
    return retval;
}

} // namespace common
} // namespace lbug

#include "common/types/string_t.h"

namespace lbug {
namespace common {

string_t::string_t(const char* value, uint64_t length) : len(length), prefix{} {
    if (isShortString(length)) {
        memcpy(prefix, value, length);
        return;
    }
    overflowPtr = (uint64_t)(value);
    memcpy(prefix, value, PREFIX_LENGTH);
}

void string_t::set(const std::string& value) {
    set(value.data(), value.length());
}

void string_t::set(const char* value, uint64_t length) {
    if (length <= SHORT_STR_LENGTH) {
        setShortString(value, length);
    } else {
        setLongString(value, length);
    }
}

void string_t::set(const string_t& value) {
    if (value.len <= SHORT_STR_LENGTH) {
        setShortString(value);
    } else {
        setLongString(value);
    }
}

std::string string_t::getAsShortString() const {
    return std::string((char*)prefix, len);
}

std::string string_t::getAsString() const {
    return std::string(getAsStringView());
}

std::string_view string_t::getAsStringView() const {
    if (len <= SHORT_STR_LENGTH) {
        return std::string_view((char*)prefix, len);
    } else {
        return std::string_view(reinterpret_cast<char*>(overflowPtr), len);
    }
}

bool string_t::operator==(const string_t& rhs) const {
    // First compare the length and prefix of the strings.
    auto numBytesOfLenAndPrefix =
        sizeof(uint32_t) + std::min((uint64_t)len, static_cast<uint64_t>(string_t::PREFIX_LENGTH));
    if (!memcmp(this, &rhs, numBytesOfLenAndPrefix)) {
        // If length and prefix of a and b are equal, we compare the overflow buffer.
        return !memcmp(getData(), rhs.getData(), len);
    }
    return false;
}

bool string_t::operator>(const string_t& rhs) const {
    // Compare string_t up to the shared length.
    // If there is a tie, we just need to compare the std::string lengths.
    auto sharedLen = std::min(len, rhs.len);
    auto memcmpResult = memcmp(prefix, rhs.prefix,
        sharedLen <= string_t::PREFIX_LENGTH ? sharedLen : string_t::PREFIX_LENGTH);
    if (memcmpResult == 0 && len > string_t::PREFIX_LENGTH) {
        memcmpResult = memcmp(getData(), rhs.getData(), sharedLen);
    }
    if (memcmpResult == 0) {
        return len > rhs.len;
    }
    return memcmpResult > 0;
}

} // namespace common
} // namespace lbug

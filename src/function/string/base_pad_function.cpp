#include "function/string/functions/base_pad_function.h"

#include "common/exception/runtime.h"
#include <format>

using namespace lbug::common;

namespace lbug {
namespace function {

// The bytes that padOperation produces: the codepoints kept from src, then the remaining
// codepoints taken from the pad string in turn.
static uint64_t paddedSize(const string_t& src, uint32_t count, const string_t& pad) {
    auto padData = reinterpret_cast<const char*>(pad.getData());
    auto [srcBytes, srcChars] = BasePadOperation::padCountChars(count,
        reinterpret_cast<const char*>(src.getData()), src.len);
    auto padChars = count - srcChars;
    auto padCodepoints = BasePadOperation::padCountChars(UINT32_MAX, padData, pad.len).second;
    if (padCodepoints == 0) {
        return srcBytes;
    }
    auto restBytes =
        BasePadOperation::padCountChars(padChars % padCodepoints, padData, pad.len).first;
    return srcBytes + static_cast<uint64_t>(padChars / padCodepoints) * pad.len + restBytes;
}

// A width over the limit would be truncated to 32 bits by padCountChars and insertPadding.
// Below it, multi-byte codepoints can still take too many bytes.
void BasePadOperation::checkResultSize(const string_t& src, uint64_t width, const string_t& pad) {
    if (width > string_t::MAX_LENGTH ||
        paddedSize(src, static_cast<uint32_t>(width), pad) > string_t::MAX_LENGTH) {
        throw RuntimeException(std::format(
            "lpad()/rpad(): the result would exceed the maximum string size of {} bytes.",
            string_t::MAX_LENGTH));
    }
}

} // namespace function
} // namespace lbug

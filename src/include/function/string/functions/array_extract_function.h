#pragma once

#include <algorithm>

#include "common/types/string_t.h"
#include "function/list/functions/list_len_function.h"
#include "utf8proc.h"

namespace lbug {
namespace function {

struct ArrayExtract {
    // Takes the character at a 1-based index, counted from the end when negative. An index outside
    // the string gives the empty string.
    static void operation(common::string_t& str, int64_t& idx, common::string_t& result) {
        result.len = 0;
        const auto numBytes = static_cast<int64_t>(str.len);
        // Every character takes at least one byte, so this needs no character count.
        if (idx == 0 || idx > numBytes || idx < -numBytes) {
            return;
        }
        int64_t numChars = 0;
        ListLen::operation(str, numChars);
        if (idx > numChars || idx < -numChars) {
            return;
        }
        const auto charIdx = idx > 0 ? idx - 1 : numChars + idx;
        const auto data = reinterpret_cast<const char*>(str.getData());
        // The count equals the byte length when every character is one byte, and when ListLen
        // counted the bytes of an all-ASCII string, CR LF included. Either way, index by bytes to
        // agree with size().
        if (numChars == numBytes) {
            result.set(data + charIdx, 1);
            return;
        }
        int64_t charCount = 0;
        utf8proc::utf8proc_grapheme_callback(data, str.len, [&](size_t start, size_t end) {
            if (charCount++ < charIdx) {
                return true;
            }
            // On malformed UTF-8 the last character can end past the string.
            result.set(data + start, std::min<size_t>(end, str.len) - start);
            return false;
        });
    }
};

} // namespace function
} // namespace lbug

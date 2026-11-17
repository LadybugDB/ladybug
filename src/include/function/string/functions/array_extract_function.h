#pragma once

#include <algorithm>

#include "common/types/string_t.h"
#include "common/vector/value_vector.h"
#include "function/list/functions/list_len_function.h"
#include "function/string/functions/substr_function.h"
#include "utf8proc.h"

namespace lbug {
namespace function {

struct ArrayExtract {
    // Takes the character at a 1-based index, counted from the end when negative. An index outside
    // the string gives the empty string.
    static inline void operation(common::string_t& str, int64_t& idx, common::string_t& result,
        common::ValueVector& resultValueVector) {
        result.len = 0;
        const auto numBytes = static_cast<int64_t>(str.len);
        // Every character takes at least one byte, so this needs no character count.
        // The guards negate numBytes/numChars (small), never idx, so INT64_MIN is safe.
        // When numBytes == 0 every idx trips one disjunct, so the grapheme walk below never
        // sees len == 0 (utf8proc_grapheme_callback dereferences data[0] even at len 0).
        if (idx == 0 || idx > numBytes || idx < -numBytes) {
            return;
        }
        int64_t numChars = 0;
        ListLen::operation(str, numChars);
        if (idx > numChars || idx < -numChars) {
            return;
        }
        const auto charIdx = idx > 0 ? idx - 1 : numChars + idx;
        // The count equals the byte length when every character is one byte, and when ListLen
        // counted the bytes of an all-ASCII string, CR LF included. Either way, index by bytes to
        // agree with size().
        if (numChars == numBytes) {
            SubStr::copySubstr(str, charIdx + 1, 1 /* length */, result, resultValueVector,
                true /* isAscii */);
            return;
        }
        const auto data = reinterpret_cast<const char*>(str.getData());
        int64_t charCount = 0;
        int64_t startBytePos = 0;
        int64_t length = 0;
        lbug::utf8proc::utf8proc_grapheme_callback(data, str.len, [&](size_t start, size_t end) {
            if (charCount++ < charIdx) {
                return true;
            }
            // On malformed UTF-8 the last character can end past the string.
            startBytePos = static_cast<int64_t>(start);
            length = static_cast<int64_t>(std::min<size_t>(end, str.len)) - startBytePos;
            return false;
        });
        // The walk always hits given the guards; a zero length stays empty.
        // copySubstr routes >12-byte clusters through the result vector's overflow buffer (#1472).
        SubStr::copySubstr(str, startBytePos, length, result, resultValueVector,
            false /* isAscii */);
    }
};

} // namespace function
} // namespace lbug

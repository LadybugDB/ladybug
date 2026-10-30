#pragma once

#include "common/types/string_t.h"
#include "common/vector/value_vector.h"
#include "function/list/functions/list_len_function.h"
#include "function/string/functions/substr_function.h"

namespace lbug {
namespace function {

struct ArrayExtract {
    static inline void operation(common::string_t& str, int64_t& idx, common::string_t& result,
        common::ValueVector& resultValueVector) {
        if (idx == 0) {
            result.len = 0;
            return;
        }
        auto stringVal = str.getAsString();
        int64_t strLen = 0;
        ListLen::operation(str, strLen);
        auto idxPos = idx > 0 ? std::min(idx, strLen) : std::max(strLen + idx, (int64_t)0) + 1;
        auto startPos = idxPos - 1;
        auto endPos = startPos + 1;
        bool isAscii = true;
        for (auto i = 0u; i < std::min((size_t)idxPos + 1, stringVal.size()); i++) {
            if (stringVal[i] & 0x80) {
                isAscii = false;
                break;
            }
        }
        if (isAscii) {
            SubStr::copySubstr(str, idxPos, 1 /* length */, result, resultValueVector, isAscii);
        } else {
            int64_t characterCount = 0, startBytePos = 0, endBytePos = 0;
            lbug::utf8proc::utf8proc_grapheme_callback(stringVal.c_str(), stringVal.size(),
                [&](int64_t gstart, int64_t /*gend*/) {
                    if (characterCount == startPos) {
                        startBytePos = gstart;
                    } else if (characterCount == endPos) {
                        endBytePos = gstart;
                        return false;
                    }
                    characterCount++;
                    return true;
                });
            if (endBytePos == 0) {
                endBytePos = str.len;
            }
            SubStr::copySubstr(str, startBytePos, endBytePos - startBytePos, result,
                resultValueVector, isAscii);
        }
    }
};

} // namespace function
} // namespace lbug

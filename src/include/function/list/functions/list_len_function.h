#pragma once

#include <cstring>

#include "common/types/string_t.h"
#include "utf8proc.h"

namespace lbug {
namespace function {

struct ListLen {
public:
    template<typename T>
    static void operation(T& input, int64_t& result) {
        result = input.size;
    }
};

template<>
inline void ListLen::operation(common::string_t& input, int64_t& result) {
    auto totalByteLength = input.len;
    auto data = reinterpret_cast<const char*>(input.getData());
    for (auto i = 0u; i < totalByteLength; i++) {
        if (data[i] & 0x80) {
            int64_t length = 0;
            // Use grapheme iterator to identify bytes of utf8 char and increment once for each
            // char.
            utf8proc::utf8proc_grapheme_callback(data, totalByteLength,
                [&](size_t /*start*/, size_t /*end*/) {
                    length++;
                    return true;
                });
            result = length;
            return;
        }
    }
    result = totalByteLength;
}

} // namespace function
} // namespace lbug

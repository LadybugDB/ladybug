#pragma once

#include "common/api.h"
#include "common/types/string_t.h"
#include "common/vector/value_vector.h"
#include "utf8proc.h"

namespace lbug {
namespace function {

// Padding logic has been taken from DuckDB:
// https://github.com/duckdb/duckdb/blob/master/src/function/scalar/string/pad.cpp
struct BasePadOperation {
public:
    static inline void operation(common::string_t& src, int64_t count,
        common::string_t& characterToPad, common::string_t& result,
        common::ValueVector& resultValueVector,
        void (*padOperation)(common::string_t& src, int64_t count, common::string_t& characterToPad,
            std::string& paddedResult)) {
        if (count < 0) {
            count = 0;
        }
        // An empty pad adds nothing, so a width past the source gives the source.
        if (characterToPad.len == 0) {
            count = std::min<int64_t>(count, src.len);
        }
        // The result can exceed MAX_LENGTH only if src.len + MAX_BYTES_PER_CODEPOINT * width
        // does. The width is checked first, so that the product cannot overflow.
        const auto width = static_cast<uint64_t>(count);
        if (width > common::string_t::MAX_LENGTH ||
            src.len + MAX_BYTES_PER_CODEPOINT * width > common::string_t::MAX_LENGTH) [[unlikely]] {
            checkResultSize(src, width, characterToPad);
        }
        std::string paddedResult;
        padOperation(src, count, characterToPad, paddedResult);
        common::StringVector::addString(&resultValueVector, result, paddedResult.data(),
            paddedResult.size());
    }

    static constexpr uint64_t MAX_BYTES_PER_CODEPOINT = 4;

    // Defined in base_pad_function.cpp, apart from where Lpad and Rpad are instantiated:
    // inlined into the per-row code above, it slows that code down although it rarely runs.
    LBUG_API static void checkResultSize(const common::string_t& src, uint64_t width,
        const common::string_t& pad);

    static std::pair<uint32_t, uint32_t> padCountChars(const uint32_t count, const char* data,
        const uint32_t size) {
        auto str = reinterpret_cast<const utf8proc::utf8proc_uint8_t*>(data);
        uint32_t byteCount = 0, charCount = 0;
        for (; charCount < count && byteCount < size; charCount++) {
            utf8proc::utf8proc_int32_t codepoint = 0;
            auto bytes = utf8proc::utf8proc_iterate(str + byteCount, size - byteCount, &codepoint);
            byteCount += bytes;
        }
        return {byteCount, charCount};
    }

    static void insertPadding(uint32_t charCount, common::string_t pad, std::string& result) {
        auto padData = pad.getData();
        auto padSize = pad.len;
        uint32_t padByteCount = 0;
        for (auto i = 0u; i < charCount; i++) {
            if (padByteCount >= padSize) {
                result.insert(result.end(), (char*)padData, (char*)(padData + padByteCount));
                padByteCount = 0;
            }
            utf8proc::utf8proc_int32_t codepoint = 0;
            auto bytes = utf8proc::utf8proc_iterate(padData + padByteCount, padSize - padByteCount,
                &codepoint);
            padByteCount += bytes;
        }
        result.insert(result.end(), (char*)padData, (char*)(padData + padByteCount));
    }
};

} // namespace function
} // namespace lbug

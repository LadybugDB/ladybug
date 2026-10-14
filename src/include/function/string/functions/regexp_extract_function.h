#pragma once

#include "common/exception/runtime.h"
#include "common/types/string_t.h"
#include "common/vector/value_vector.h"
#include "function/string/functions/base_regexp_function.h"
#include "re2.h"

namespace lbug {
namespace function {

struct RegexpExtract : BaseRegexpOperation {
    static inline void operation(common::string_t& value, common::string_t& pattern,
        std::int64_t& group, common::string_t& result, common::ValueVector& resultValueVector,
        void* dataPtr) {
        regexExtract(value.getAsString(), RegexpBindData::getRegex(dataPtr, pattern), group, result,
            resultValueVector);
    }

    static inline void operation(common::string_t& value, common::string_t& pattern,
        common::string_t& result, common::ValueVector& /*leftValueVector*/,
        common::ValueVector& /*rightValueVector*/, common::ValueVector& resultValueVector,
        void* dataPtr) {
        int64_t defaultGroup = 0;
        regexExtract(value.getAsString(), RegexpBindData::getRegex(dataPtr, pattern), defaultGroup,
            result, resultValueVector);
    }

    static void regexExtract(const std::string& input, const RE2& regex, std::int64_t& group,
        common::string_t& result, common::ValueVector& resultValueVector) {
        auto submatchCount = regex.NumberOfCapturingGroups() + 1;
        if (group < 0 || group >= submatchCount) {
            throw common::RuntimeException("Regex match group index is out of range");
        }

        std::vector<regex::StringPiece> targetSubMatches;
        targetSubMatches.resize(submatchCount);

        if (!regex.Match(regex::StringPiece(input), 0, input.length(), RE2::UNANCHORED,
                targetSubMatches.data(), submatchCount)) {
            return;
        }

        copyToLbugString(targetSubMatches[group].ToString(), result, resultValueVector);
    }
};

} // namespace function
} // namespace lbug

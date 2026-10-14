#pragma once

#include "base_regexp_function.h"
#include "common/vector/value_vector.h"
#include "re2.h"

namespace lbug {
namespace function {

struct RegexpSplitToArray : BaseRegexpOperation {
    static void operation(common::string_t& value, common::string_t& pattern,
        common::list_entry_t& result, common::ValueVector& /*leftValueVector*/,
        common::ValueVector& /*rightValueVector*/, common::ValueVector& resultVector,
        void* dataPtr) {
        std::vector<std::string> matches =
            regexExtractAll(value.getAsString(), RegexpBindData::getRegex(dataPtr, pattern));
        result = common::ListVector::addList(&resultVector, matches.size());
        auto resultValues = common::ListVector::getListValues(&resultVector, result);
        auto resultDataVector = common::ListVector::getDataVector(&resultVector);
        auto numBytesPerValue = resultDataVector->getNumBytesPerValue();
        common::string_t str;
        for (const auto& match : matches) {
            copyToLbugString(match, str, *resultDataVector);
            resultDataVector->copyFromVectorData(resultValues, resultDataVector,
                reinterpret_cast<uint8_t*>(&str));
            resultValues += numBytesPerValue;
        }
    }

    static std::vector<std::string> regexExtractAll(const std::string& value, const RE2& regex) {

        regex::StringPiece input(value);
        regex::StringPiece match;
        uint64_t startPos = 0;

        std::vector<std::string> splitParts;
        while (startPos < input.length()) {
            if (regex.Match(input, startPos, input.length(), RE2::UNANCHORED, &match, 1)) {
                uint64_t matchStart = match.data() - input.data();
                uint64_t matchEnd = matchStart + match.size();

                if (startPos < matchStart) {
                    splitParts.emplace_back(value.substr(startPos, matchStart - startPos));
                }

                startPos = matchEnd;

                if (match.size() == 0) {
                    // Match size is 0.
                    startPos++;
                }
            } else {
                // No more regexp matches.
                if (startPos < input.length()) {
                    splitParts.emplace_back(value.substr(startPos));
                }
                break;
            }
        }
        return splitParts;
    }
};

} // namespace function
} // namespace lbug
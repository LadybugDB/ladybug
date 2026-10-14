#pragma once

#include "common/types/string_t.h"
#include "function/string/functions/base_regexp_function.h"
#include "re2.h"

namespace lbug {
namespace function {

struct RegexpMatches : BaseRegexpOperation {
    static void operation(common::string_t& left, common::string_t& right, uint8_t& result,
        common::ValueVector& /*leftValueVector*/, common::ValueVector& /*rightValueVector*/,
        common::ValueVector& /*resultValueVector*/, void* dataPtr) {
        result = RE2::PartialMatch(toStringPiece(left), RegexpBindData::getRegex(dataPtr, right));
    }
};

} // namespace function
} // namespace lbug

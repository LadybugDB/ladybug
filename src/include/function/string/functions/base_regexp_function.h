#pragma once

#include <memory>
#include <regex>
#include <string>

#include "common/vector/value_vector.h"
#include "function/function.h"
#include "re2.h"

namespace lbug {
namespace function {

struct BaseRegexpOperation {
    static inline std::string parseCypherPattern(const std::string& pattern) {
        // Cypher parses escape characters with 2 backslash eg. for expressing '.' requires '\\.'
        // Since Regular Expression requires only 1 backslash '\.' we need to replace double slash
        // with single
        return std::regex_replace(pattern, std::regex(R"(\\\\)"), "\\");
    }

    static inline void copyToLbugString(const std::string& value, common::string_t& lbugString,
        common::ValueVector& valueVector) {
        common::StringVector::addString(&valueVector, lbugString, value.data(), value.length());
    }

    static regex::StringPiece toStringPiece(const common::string_t& value) {
        return regex::StringPiece(reinterpret_cast<const char*>(value.getData()), value.len);
    }

    // Only a STRING literal can be compiled at bind time: the binder casts a literal of any other
    // type to STRING after the bind function returns, so such a pattern is compiled per row.
    static bool isStringLiteral(const binder::Expression& pattern) {
        return pattern.expressionType == common::ExpressionType::LITERAL &&
               pattern.getDataType().getLogicalTypeID() == common::LogicalTypeID::STRING;
    }
};

// Keeps the last compiled pattern and recompiles only when the pattern changes, so a pattern
// that is the same on every row (a literal or a parameter) is compiled once, not once per row.
// Evaluators copy their bind data, so each evaluator owns its cache. An invalid pattern is cached
// too; RE2 then reports no match, as it did when the pattern was compiled per row.
struct RegexpBindData : FunctionBindData {
    RegexpBindData(common::logical_type_vec_t paramTypes, common::LogicalType resultType,
        bool parseCypherEscapes = true)
        : FunctionBindData{std::move(paramTypes), std::move(resultType)},
          parseCypherEscapes{parseCypherEscapes} {}

    static const RE2& getRegex(void* dataPtr, const common::string_t& pattern) {
        auto& bindData = *reinterpret_cast<RegexpBindData*>(dataPtr);
        auto patternStr = pattern.getAsStringView();
        if (bindData.regex == nullptr || patternStr != bindData.source) {
            bindData.source = patternStr;
            bindData.regex =
                std::make_unique<RE2>(bindData.parseCypherEscapes ?
                                          BaseRegexpOperation::parseCypherPattern(bindData.source) :
                                          bindData.source);
        }
        return *bindData.regex;
    }

    std::unique_ptr<FunctionBindData> copy() const override {
        return std::make_unique<RegexpBindData>(copyVector(paramTypes), resultType.copy(),
            parseCypherEscapes);
    }

private:
    bool parseCypherEscapes;
    std::string source;
    std::unique_ptr<RE2> regex;
};

} // namespace function
} // namespace lbug

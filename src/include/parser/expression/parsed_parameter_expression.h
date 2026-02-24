#pragma once

#include "common/assert.h"
#include "parsed_expression.h"

namespace lbug {
namespace parser {

class ParsedParameterExpression : public ParsedExpression {
public:
    explicit ParsedParameterExpression(std::string parameterName, std::string raw)
        : ParsedExpression{common::ExpressionType::PARAMETER, std::move(raw)},
          parameterName{std::move(parameterName)} {}

    inline std::string getParameterName() const { return parameterName; }

    static std::unique_ptr<ParsedParameterExpression> deserialize(common::Deserializer&) {
        UNREACHABLE_CODE;
    }

    inline std::unique_ptr<ParsedExpression> copy() const override { UNREACHABLE_CODE; }

private:
    void serializeInternal(common::Serializer&) const override { UNREACHABLE_CODE; }

private:
    std::string parameterName;
};

} // namespace parser
} // namespace lbug

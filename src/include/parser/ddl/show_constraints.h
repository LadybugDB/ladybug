#pragma once

#include "parser/statement.h"

namespace lbug {
namespace parser {

// Parsed info for: SHOW CONSTRAINTS
class ShowConstraints final : public Statement {
    static constexpr common::StatementType type_ = common::StatementType::SHOW_CONSTRAINTS;

public:
    ShowConstraints() : Statement{type_} {}
};

} // namespace parser
} // namespace lbug

#pragma once

#include "binder/bound_statement.h"

namespace lbug {
namespace binder {

class BoundGraphStatement : public BoundStatement {
public:
    explicit BoundGraphStatement(common::StatementType statementType, std::string graphName)
        : BoundStatement{statementType, BoundStatementResult::createSingleStringColumnResult()},
          graphName{std::move(graphName)} {}

    std::string getGraphName() const { return graphName; }

private:
    std::string graphName;
};

class BoundCreateGraph final : public BoundGraphStatement {
    static constexpr common::StatementType type_ = common::StatementType::CREATE_GRAPH;

public:
    explicit BoundCreateGraph(std::string graphName)
        : BoundGraphStatement{type_, std::move(graphName)} {}
};

class BoundUseGraph final : public BoundGraphStatement {
    static constexpr common::StatementType type_ = common::StatementType::USE_GRAPH;

public:
    explicit BoundUseGraph(std::string graphName)
        : BoundGraphStatement{type_, std::move(graphName)} {}
};

} // namespace binder
} // namespace lbug

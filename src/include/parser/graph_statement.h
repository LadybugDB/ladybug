#pragma once

#include "parser/statement.h"

namespace lbug {
namespace parser {

class GraphStatement : public Statement {
public:
    explicit GraphStatement(common::StatementType type, std::string graphName)
        : Statement{type}, graphName{std::move(graphName)} {}

    std::string getGraphName() const { return graphName; }

private:
    std::string graphName;
};

class CreateGraph final : public GraphStatement {
public:
    explicit CreateGraph(std::string graphName)
        : GraphStatement{common::StatementType::CREATE_GRAPH, std::move(graphName)} {}
};

class UseGraph final : public GraphStatement {
public:
    explicit UseGraph(std::string graphName)
        : GraphStatement{common::StatementType::USE_GRAPH, std::move(graphName)} {}
};

} // namespace parser
} // namespace lbug

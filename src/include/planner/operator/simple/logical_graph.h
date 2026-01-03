#pragma once

#include "logical_simple.h"

namespace lbug {
namespace planner {

class LogicalCreateGraph final : public LogicalSimple {
    static constexpr LogicalOperatorType type_ = LogicalOperatorType::CREATE_GRAPH;

public:
    explicit LogicalCreateGraph(std::string graphName)
        : LogicalSimple{type_}, graphName{std::move(graphName)} {}

    std::string getGraphName() const { return graphName; }

    std::string getExpressionsForPrinting() const override { return graphName; }

    std::unique_ptr<LogicalOperator> copy() override {
        return std::make_unique<LogicalCreateGraph>(graphName);
    }

private:
    std::string graphName;
};

class LogicalUseGraph final : public LogicalSimple {
    static constexpr LogicalOperatorType type_ = LogicalOperatorType::USE_GRAPH;

public:
    explicit LogicalUseGraph(std::string graphName)
        : LogicalSimple{type_}, graphName{std::move(graphName)} {}

    std::string getGraphName() const { return graphName; }

    std::string getExpressionsForPrinting() const override { return graphName; }

    std::unique_ptr<LogicalOperator> copy() override {
        return std::make_unique<LogicalUseGraph>(graphName);
    }

private:
    std::string graphName;
};

} // namespace planner
} // namespace lbug

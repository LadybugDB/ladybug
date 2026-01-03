#pragma once

#include "processor/operator/sink.h"

namespace lbug {
namespace processor {

struct UseGraphPrintInfo final : OPPrintInfo {
    std::string graphName;

    explicit UseGraphPrintInfo(std::string graphName) : graphName(std::move(graphName)) {}

    std::string toString() const override {
        std::string result = "Graph: ";
        result += graphName;
        return result;
    }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::unique_ptr<UseGraphPrintInfo>(new UseGraphPrintInfo(*this));
    }
};

class UseGraph final : public SimpleSink {
public:
    UseGraph(std::string graphName, std::shared_ptr<FactorizedTable> messageTable,
        physical_op_id id, std::unique_ptr<OPPrintInfo> printInfo)
        : SimpleSink{PhysicalOperatorType::USE_GRAPH, std::move(messageTable), id,
              std::move(printInfo)},
          graphName(std::move(graphName)) {}

    std::string getGraphName() const { return graphName; }

    void executeInternal(ExecutionContext* context) override;
    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<UseGraph>(graphName, messageTable, id, printInfo->copy());
    }

private:
    std::string graphName;
};

class CreateGraph final : public SimpleSink {
public:
    CreateGraph(std::string graphName, std::shared_ptr<FactorizedTable> messageTable,
        physical_op_id id, std::unique_ptr<OPPrintInfo> printInfo)
        : SimpleSink{PhysicalOperatorType::CREATE_GRAPH, std::move(messageTable), id,
              std::move(printInfo)},
          graphName(std::move(graphName)) {}

    std::string getGraphName() const { return graphName; }

    void executeInternal(ExecutionContext* context) override;
    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<CreateGraph>(graphName, messageTable, id, printInfo->copy());
    }

private:
    std::string graphName;
};

} // namespace processor
} // namespace lbug

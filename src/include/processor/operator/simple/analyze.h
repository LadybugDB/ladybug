#pragma once

#include <optional>

#include "processor/operator/sink.h"

namespace lbug {
namespace processor {

struct AnalyzePrintInfo final : OPPrintInfo {
    std::optional<std::string> tableName;

    explicit AnalyzePrintInfo(std::optional<std::string> tableName)
        : tableName{std::move(tableName)} {}

    std::string toString() const override;

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::unique_ptr<AnalyzePrintInfo>(new AnalyzePrintInfo(*this));
    }

private:
    AnalyzePrintInfo(const AnalyzePrintInfo& other)
        : OPPrintInfo(other), tableName(other.tableName) {}
};

class Analyze final : public SimpleSink {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::ANALYZE;

public:
    Analyze(std::optional<std::string> tableName, std::shared_ptr<FactorizedTable> messageTable,
        physical_op_id id, std::unique_ptr<OPPrintInfo> printInfo)
        : SimpleSink{type_, std::move(messageTable), id, std::move(printInfo)},
          tableName{std::move(tableName)} {}

    void executeInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<Analyze>(tableName, messageTable, id, printInfo->copy());
    }

private:
    std::optional<std::string> tableName;
};

} // namespace processor
} // namespace lbug

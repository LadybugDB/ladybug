#pragma once

#include "binder/ddl/bound_create_index.h"
#include "processor/operator/sink.h"

namespace lbug {
namespace processor {

class CreateIndex final : public SimpleSink {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::CREATE_INDEX;

public:
    CreateIndex(binder::BoundCreateIndexInfo info, std::shared_ptr<FactorizedTable> messageTable,
        physical_op_id id, std::unique_ptr<OPPrintInfo> printInfo)
        : SimpleSink{type_, std::move(messageTable), id, std::move(printInfo)},
          info{std::move(info)} {}

    void executeInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<CreateIndex>(info.copy(), messageTable, id, printInfo->copy());
    }

private:
    binder::BoundCreateIndexInfo info;
};

} // namespace processor
} // namespace lbug

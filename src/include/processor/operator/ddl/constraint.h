#pragma once

#include "binder/ddl/bound_create_constraint.h"
#include "binder/ddl/bound_drop_constraint.h"
#include "processor/operator/sink.h"

namespace lbug {
namespace processor {

class CreateConstraint final : public SimpleSink {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::CREATE_CONSTRAINT;

public:
    CreateConstraint(binder::BoundCreateConstraintInfo info,
        std::shared_ptr<FactorizedTable> messageTable, physical_op_id id,
        std::unique_ptr<OPPrintInfo> printInfo)
        : SimpleSink{type_, std::move(messageTable), id, std::move(printInfo)},
          info{std::move(info)} {}

    void executeInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<CreateConstraint>(info.copy(), messageTable, id, printInfo->copy());
    }

private:
    binder::BoundCreateConstraintInfo info;
};

class DropConstraint final : public SimpleSink {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::DROP_CONSTRAINT;

public:
    DropConstraint(binder::BoundDropConstraintInfo info,
        std::shared_ptr<FactorizedTable> messageTable, physical_op_id id,
        std::unique_ptr<OPPrintInfo> printInfo)
        : SimpleSink{type_, std::move(messageTable), id, std::move(printInfo)},
          info{std::move(info)} {}

    void executeInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<DropConstraint>(info, messageTable, id, printInfo->copy());
    }

private:
    binder::BoundDropConstraintInfo info;
};

} // namespace processor
} // namespace lbug

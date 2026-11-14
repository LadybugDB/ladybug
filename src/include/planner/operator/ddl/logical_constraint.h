#pragma once

#include "binder/ddl/bound_create_constraint.h"
#include "binder/ddl/bound_drop_constraint.h"
#include "planner/operator/simple/logical_simple.h"

namespace lbug {
namespace planner {

struct LogicalCreateConstraintPrintInfo final : OPPrintInfo {
    binder::BoundCreateConstraintInfo info;

    explicit LogicalCreateConstraintPrintInfo(binder::BoundCreateConstraintInfo info)
        : info{std::move(info)} {}

    std::string toString() const override { return info.toString(); }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<LogicalCreateConstraintPrintInfo>(*this);
    }
};

class LogicalCreateConstraint final : public LogicalSimple {
    static constexpr LogicalOperatorType type_ = LogicalOperatorType::CREATE_CONSTRAINT;

public:
    explicit LogicalCreateConstraint(binder::BoundCreateConstraintInfo info)
        : LogicalSimple{type_}, info{std::move(info)} {}

    std::string getExpressionsForPrinting() const override { return info.constraintName; }

    const binder::BoundCreateConstraintInfo* getInfo() const { return &info; }

    std::unique_ptr<OPPrintInfo> getPrintInfo() const override {
        return std::make_unique<LogicalCreateConstraintPrintInfo>(info.copy());
    }

    std::unique_ptr<LogicalOperator> copy() override {
        return std::make_unique<LogicalCreateConstraint>(info.copy());
    }

private:
    binder::BoundCreateConstraintInfo info;
};

struct LogicalDropConstraintPrintInfo final : OPPrintInfo {
    binder::BoundDropConstraintInfo info;

    explicit LogicalDropConstraintPrintInfo(binder::BoundDropConstraintInfo info)
        : info{std::move(info)} {}

    std::string toString() const override { return info.constraintName; }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<LogicalDropConstraintPrintInfo>(*this);
    }
};

class LogicalDropConstraint final : public LogicalSimple {
    static constexpr LogicalOperatorType type_ = LogicalOperatorType::DROP_CONSTRAINT;

public:
    explicit LogicalDropConstraint(binder::BoundDropConstraintInfo info)
        : LogicalSimple{type_}, info{std::move(info)} {}

    std::string getExpressionsForPrinting() const override { return info.constraintName; }

    const binder::BoundDropConstraintInfo* getInfo() const { return &info; }

    std::unique_ptr<OPPrintInfo> getPrintInfo() const override {
        return std::make_unique<LogicalDropConstraintPrintInfo>(info);
    }

    std::unique_ptr<LogicalOperator> copy() override {
        return std::make_unique<LogicalDropConstraint>(info);
    }

private:
    binder::BoundDropConstraintInfo info;
};

} // namespace planner
} // namespace lbug

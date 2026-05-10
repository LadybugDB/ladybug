#pragma once

#include "binder/ddl/bound_create_index.h"
#include "planner/operator/simple/logical_simple.h"

namespace lbug {
namespace planner {

struct LogicalCreateIndexPrintInfo final : OPPrintInfo {
    binder::BoundCreateIndexInfo info;

    explicit LogicalCreateIndexPrintInfo(binder::BoundCreateIndexInfo info)
        : info{std::move(info)} {}

    std::string toString() const override { return info.toString(); }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<LogicalCreateIndexPrintInfo>(*this);
    }
};

class LogicalCreateIndex final : public LogicalSimple {
    static constexpr LogicalOperatorType type_ = LogicalOperatorType::CREATE_INDEX;

public:
    explicit LogicalCreateIndex(binder::BoundCreateIndexInfo info)
        : LogicalSimple{type_}, info{std::move(info)} {}

    std::string getExpressionsForPrinting() const override { return info.indexName; }

    const binder::BoundCreateIndexInfo* getInfo() const { return &info; }

    std::unique_ptr<OPPrintInfo> getPrintInfo() const override {
        return std::make_unique<LogicalCreateIndexPrintInfo>(info.copy());
    }

    std::unique_ptr<LogicalOperator> copy() override {
        return std::make_unique<LogicalCreateIndex>(info.copy());
    }

private:
    binder::BoundCreateIndexInfo info;
};

} // namespace planner
} // namespace lbug

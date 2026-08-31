#pragma once

#include "function/gds/rec_joins.h"
#include "processor/operator/sink.h"

namespace lbug {
namespace processor {

struct RecursiveExtendPrintInfo final : OPPrintInfo {
    std::string funcName;

    explicit RecursiveExtendPrintInfo(std::string funcName) : funcName{std::move(funcName)} {}

    std::string toString() const override { return funcName; }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::unique_ptr<RecursiveExtendPrintInfo>(new RecursiveExtendPrintInfo(*this));
    }

private:
    RecursiveExtendPrintInfo(const RecursiveExtendPrintInfo& other)
        : OPPrintInfo{other}, funcName{other.funcName} {}
};

class RecursiveExtend : public Sink {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::RECURSIVE_EXTEND;

public:
    RecursiveExtend(std::unique_ptr<function::RJAlgorithm> function, function::RJBindData bindData,
        std::shared_ptr<RecursiveExtendSharedState> sharedState, uint32_t id,
        std::unique_ptr<OPPrintInfo> printInfo)
        : Sink{type_, id, std::move(printInfo)}, function{std::move(function)}, bindData{bindData},
          sharedState{std::move(sharedState)} {}

    std::shared_ptr<RecursiveExtendSharedState> getSharedState() const { return sharedState; }

    bool isSource() const override { return true; }

    bool isParallel() const override { return false; }

    void executeInternal(ExecutionContext* context) override;

    // The reset must happen in prepareForReuse() rather than initGlobalStateInternal(): this
    // operator's task only starts after the semi-masker child pipeline has filled the node
    // offset masks, so resetting there would run too late. prepareForReuse() runs on the whole
    // plan before any task of the new execution starts.
    void prepareForReuse(storage::MemoryManager* memoryManager) override {
        sharedState->resetForReuse();
        PhysicalOperator::prepareForReuse(memoryManager);
    }

    std::unique_ptr<PhysicalOperator> copy() override {
        auto result = std::make_unique<RecursiveExtend>(function->copy(), bindData, sharedState, id,
            printInfo->copy());
        for (auto& child : children) {
            result->addChild(child->copy());
        }
        return result;
    }

private:
    std::unique_ptr<function::RJAlgorithm> function;
    function::RJBindData bindData;
    std::shared_ptr<RecursiveExtendSharedState> sharedState;
};

} // namespace processor
} // namespace lbug

#pragma once

#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "processor/operator/physical_operator.h"

namespace lbug {
namespace processor {

struct UnwindDedupPrintInfo final : OPPrintInfo {
    std::string toString() const override { return "UnwindDedup"; }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<UnwindDedupPrintInfo>(*this);
    }
};

class UnwindDedup final : public PhysicalOperator {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::UNWIND_DEDUP;

public:
    UnwindDedup(std::vector<DataPos> keyDataPositions, std::unique_ptr<PhysicalOperator> child,
        uint32_t id, std::unique_ptr<OPPrintInfo> printInfo)
        : PhysicalOperator{type_, std::move(child), id, std::move(printInfo)},
          keyDataPositions{std::move(keyDataPositions)} {}

    bool getNextTuplesInternal(ExecutionContext* context) override;

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return make_unique<UnwindDedup>(keyDataPositions, children[0]->copy(), id,
            printInfo->copy());
    }

private:
    std::vector<DataPos> keyDataPositions;
    std::vector<common::ValueVector*> keyVectors;
    std::unordered_set<common::hash_t> seenHashes;
};

} // namespace processor
} // namespace lbug

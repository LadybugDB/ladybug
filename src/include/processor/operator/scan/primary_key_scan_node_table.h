#pragma once

#include "expression_evaluator/expression_evaluator.h"
#include "processor/operator/scan/scan_node_table.h"

namespace lbug {
namespace processor {

struct PrimaryKeyScanPrintInfo final : OPPrintInfo {
    binder::expression_vector expressions;
    std::string key;
    std::string alias;
    std::string indexType;

    PrimaryKeyScanPrintInfo(binder::expression_vector expressions, std::string key,
        std::string alias, std::string indexType)
        : expressions(std::move(expressions)), key(std::move(key)), alias{std::move(alias)},
          indexType{std::move(indexType)} {}

    std::string toString() const override;

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::unique_ptr<PrimaryKeyScanPrintInfo>(new PrimaryKeyScanPrintInfo(*this));
    }

private:
    PrimaryKeyScanPrintInfo(const PrimaryKeyScanPrintInfo& other)
        : OPPrintInfo(other), expressions(other.expressions), key(other.key), alias(other.alias),
          indexType(other.indexType) {}
};

struct PrimaryKeyScanSharedState {
    std::mutex mtx;

    common::idx_t numTables;
    common::idx_t cursor;

    explicit PrimaryKeyScanSharedState(common::idx_t numTables) : numTables{numTables}, cursor{0} {}

    common::idx_t getTableIdx();
};

class PrimaryKeyScanNodeTable : public ScanTable {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::PRIMARY_KEY_SCAN_NODE_TABLE;

public:
    PrimaryKeyScanNodeTable(ScanOpInfo opInfo, std::vector<ScanNodeTableInfo> tableInfos,
        std::unique_ptr<evaluator::ExpressionEvaluator> indexEvaluator,
        std::unique_ptr<evaluator::ExpressionEvaluator> upperBoundEvaluator, bool isRange,
        bool isIndexEquality, bool lowerInclusive, bool upperInclusive, std::string indexName,
        std::shared_ptr<PrimaryKeyScanSharedState> sharedState, physical_op_id id,
        std::unique_ptr<OPPrintInfo> printInfo)
        : ScanTable{type_, std::move(opInfo), id, std::move(printInfo)}, scanState{nullptr},
          tableInfos{std::move(tableInfos)}, indexEvaluator{std::move(indexEvaluator)},
          upperBoundEvaluator{std::move(upperBoundEvaluator)}, sharedState{std::move(sharedState)},
          isRange{isRange}, isIndexEquality{isIndexEquality}, lowerInclusive{lowerInclusive},
          upperInclusive{upperInclusive}, indexName{std::move(indexName)}, currentRangeTableIdx{0},
          rangeOffsetCursor{0} {}

    bool isSource() const override { return true; }

    void initLocalStateInternal(ResultSet*, ExecutionContext*) override;

    bool getNextTuplesInternal(ExecutionContext* context) override;

    bool isParallel() const override { return false; }

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<PrimaryKeyScanNodeTable>(opInfo.copy(), copyVector(tableInfos),
            indexEvaluator == nullptr ? nullptr : indexEvaluator->copy(),
            upperBoundEvaluator == nullptr ? nullptr : upperBoundEvaluator->copy(), isRange,
            isIndexEquality, lowerInclusive, upperInclusive, indexName, sharedState, id,
            printInfo->copy());
    }

private:
    bool lookupRange(ExecutionContext* context);

private:
    std::unique_ptr<storage::NodeTableScanState> scanState;
    std::vector<ScanNodeTableInfo> tableInfos;
    std::unique_ptr<evaluator::ExpressionEvaluator> indexEvaluator;
    std::unique_ptr<evaluator::ExpressionEvaluator> upperBoundEvaluator;
    std::shared_ptr<PrimaryKeyScanSharedState> sharedState;
    bool isRange;
    bool isIndexEquality;
    bool lowerInclusive;
    bool upperInclusive;
    std::string indexName;
    common::idx_t currentRangeTableIdx;
    std::vector<common::offset_t> rangeOffsets;
    common::idx_t rangeOffsetCursor;
};

} // namespace processor
} // namespace lbug

#pragma once

#include <mutex>
#include <optional>

#include "common/arrow/arrow.h"
#include "main/query_result/arrow_query_result.h"
#include "processor/operator/sink.h"
#include "processor/result/flat_tuple.h"

namespace lbug {
namespace processor {

class ArrowResultCollectorSharedState {
public:
    std::vector<ArrowArray> arrays;
    std::optional<main::ArrowQueryResult::CSRMetadata> csrMetadata;

    void merge(const std::vector<ArrowArray>& localArrays,
        const std::optional<main::ArrowQueryResult::CSRMetadata>& localCSRMetadata);

private:
    std::mutex mutex;
};

struct CSRTrackingInfo {
    common::idx_t srcRowIDColIdx = common::INVALID_IDX;
    common::idx_t dstRowIDColIdx = common::INVALID_IDX;
    common::idx_t relRowIDColIdx = common::INVALID_IDX;

    bool enabled() const {
        return srcRowIDColIdx != common::INVALID_IDX && dstRowIDColIdx != common::INVALID_IDX;
    }
    bool hasRelRowID() const { return relRowIDColIdx != common::INVALID_IDX; }
};

struct ArrowResultCollectorLocalState {
    std::vector<ArrowArray> arrays;
    std::vector<common::ValueVector*> vectors;
    std::vector<std::reference_wrapper<common::sel_t>> vectorsSelPos;
    std::vector<common::DataChunk*> chunks;
    std::vector<common::sel_t> chunkCursors;
    std::unique_ptr<FlatTuple> tuple;
    std::optional<main::ArrowQueryResult::CSRMetadata> csrMetadata;
    int64_t nextSourceRowID = 0;
    int64_t currentSourceRowID = -1;
    bool csrMetadataValid = true;

    // Advance cursor.
    bool advance();
    // Scan from vector to tuple based on cursor.
    void fillTuple();

    void resetCursor();
};

struct ArrowResultCollectorInfo {
    int64_t chunkSize;
    std::vector<DataPos> payloadPositions;
    std::vector<common::LogicalType> columnTypes;
    CSRTrackingInfo csrTrackingInfo;

    ArrowResultCollectorInfo(int64_t chunkSize, std::vector<DataPos> payloadPositions,
        std::vector<common::LogicalType> columnTypes, CSRTrackingInfo csrTrackingInfo = {})
        : chunkSize{chunkSize}, payloadPositions{std::move(payloadPositions)},
          columnTypes{std::move(columnTypes)}, csrTrackingInfo{csrTrackingInfo} {}
    EXPLICIT_COPY_DEFAULT_MOVE(ArrowResultCollectorInfo);

private:
    ArrowResultCollectorInfo(const ArrowResultCollectorInfo& other)
        : chunkSize{other.chunkSize}, payloadPositions{other.payloadPositions},
          columnTypes{copyVector(other.columnTypes)}, csrTrackingInfo{other.csrTrackingInfo} {}
};

class ArrowResultCollector final : public Sink {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::RESULT_COLLECTOR;

public:
    ArrowResultCollector(std::shared_ptr<ArrowResultCollectorSharedState> sharedState,
        ArrowResultCollectorInfo info, std::unique_ptr<PhysicalOperator> child, physical_op_id id,
        std::unique_ptr<OPPrintInfo> printInfo)
        : Sink{type_, std::move(child), id, std::move(printInfo)},
          sharedState{std::move(sharedState)}, info{std::move(info)} {}

    std::unique_ptr<main::QueryResult> getQueryResult() const override;

    void executeInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<ArrowResultCollector>(sharedState, info.copy(), children[0]->copy(),
            id, printInfo->copy());
    }

private:
    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext*) override;

    void iterateResultSet(common::ArrowRowBatch* inputBatch);
    bool fillRowBatch(common::ArrowRowBatch& rowBatch);

private:
    std::shared_ptr<ArrowResultCollectorSharedState> sharedState;
    ArrowResultCollectorInfo info;
    ArrowResultCollectorLocalState localState;
};

} // namespace processor
} // namespace lbug

#pragma once

#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <optional>

#include "common/arrow/arrow.h"
#include "main/query_result/arrow_query_result.h"
#include "processor/operator/physical_operator.h"
#include "processor/operator/sink.h"
#include "processor/result/flat_tuple.h"

namespace lbug {
namespace processor {

// Globally unique, monotonically increasing identifier assigned to each
// per-thread local collector. Each thread of the parallel collector is
// associated with one batch_index, and the final result is built by walking
// chunks in batch_index order.
using batch_index_t = uint64_t;
static constexpr batch_index_t INVALID_BATCH_INDEX = std::numeric_limits<batch_index_t>::max();

// Atomic counter that hands out unique batch_index values. One instance is
// owned by ArrowResultCollectorSharedState and shared across all local
// collectors. The atomic fetch_add is the only synchronization needed to
// give each thread a distinct batch_index.
class BatchIndexAssigner {
public:
    batch_index_t next() { return counter_.fetch_add(1, std::memory_order_relaxed); }
    batch_index_t peek() const { return counter_.load(std::memory_order_relaxed); }

private:
    std::atomic<batch_index_t> counter_{0};
};

class ArrowResultCollectorSharedState {
public:
    // Per-batch Arrow chunks (parallel collector, no pairwise merge). The
    // map's natural ordering on batch_index_t gives us the correct global
    // row order at result-construction time without sorting.
    std::map<batch_index_t, std::vector<ArrowArray>> arraysByBatchIndex;
    // Per-batch CSR metadata. One entry per batch that contributed CSR
    // metadata. Absent entries are simply not in the map.
    std::map<batch_index_t, main::ArrowQueryResult::CSRMetadata> csrMetadataByBatchIndex;
    // When true, the query has a FIXED_ORDER operator (ORDER BY / TopK) on
    // the data path. Merge keeps a single running chunk under key 0 via the
    // existing pairwise mergeCSRMetadata, preserving global sort order.
    // When false, merge moves per-batch chunks into arraysByBatchIndex
    // without pairwise merge — the cheap batch-index path.
    bool requireDeterministicOrder = false;
    BatchIndexAssigner batchIndexAssigner;

    // Local collector hands off its arrays (taken by value so the move
    // out of the local state is zero-copy), the batch_index it was assigned
    // at init, and its CSR metadata (also by value for zero-copy move).
    void merge(std::vector<ArrowArray> localArrays, batch_index_t batchIndex,
        std::optional<main::ArrowQueryResult::CSRMetadata> localCSRMetadata);

private:
    std::mutex mutex;
};

struct CSRTrackingInfo {
    common::idx_t srcRowIDColIdx = common::INVALID_IDX;
    common::idx_t dstRowIDColIdx = common::INVALID_IDX;
    common::idx_t relRowIDColIdx = common::INVALID_IDX;
    // Total number of source (node) rows. Set at plan-mapping time from
    // the node table cardinality. Used to fill trailing empty rows in the
    // per-chunk CSR indptr so that indptr always has numSourceRows + 1
    // entries (one per node row plus trailing sentinel).
    int64_t numSourceRows = 0;

    bool enabled() const {
        return srcRowIDColIdx != common::INVALID_IDX && dstRowIDColIdx != common::INVALID_IDX;
    }
    bool hasRelRowID() const { return relRowIDColIdx != common::INVALID_IDX; }
};

struct ArrowResultCollectorLocalState {
    // Batch index assigned at initLocalStateInternal from the shared
    // BatchIndexAssigner. INVALID_BATCH_INDEX until then.
    batch_index_t batchIndex = INVALID_BATCH_INDEX;
    std::vector<ArrowArray> arrays;
    std::vector<common::ValueVector*> vectors;
    std::vector<std::reference_wrapper<common::sel_t>> vectorsSelPos;
    std::vector<common::DataChunk*> chunks;
    std::vector<common::sel_t> chunkCursors;
    std::unique_ptr<FlatTuple> tuple;
    std::optional<main::ArrowQueryResult::CSRMetadata> csrMetadata;
    // Current source row being accumulated and how many edges have been
    // seen for it. Used by the sparse per-batch CSR builder to emit a
    // (srcRow, count) run when the source row changes.
    int64_t currentSourceRowID = -1;
    int64_t currentRowCount = 0;
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
    // Order-preservation type of the physical plan (decided by
    // PhysicalPlanUtil::getOrderPreservation at plan-mapping time).
    OrderPreservationType orderPreservation = OrderPreservationType::NO_ORDER;

    ArrowResultCollectorInfo(int64_t chunkSize, std::vector<DataPos> payloadPositions,
        std::vector<common::LogicalType> columnTypes, CSRTrackingInfo csrTrackingInfo = {},
        OrderPreservationType orderPreservation = OrderPreservationType::NO_ORDER)
        : chunkSize{chunkSize}, payloadPositions{std::move(payloadPositions)},
          columnTypes{std::move(columnTypes)}, csrTrackingInfo{csrTrackingInfo},
          orderPreservation{orderPreservation} {}
    EXPLICIT_COPY_DEFAULT_MOVE(ArrowResultCollectorInfo);

private:
    ArrowResultCollectorInfo(const ArrowResultCollectorInfo& other)
        : chunkSize{other.chunkSize}, payloadPositions{other.payloadPositions},
          columnTypes{copyVector(other.columnTypes)}, csrTrackingInfo{other.csrTrackingInfo},
          orderPreservation{other.orderPreservation} {}
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

class DirectArrowResultCollector final : public Sink {
    static constexpr PhysicalOperatorType type_ = PhysicalOperatorType::RESULT_COLLECTOR;

public:
    DirectArrowResultCollector(std::shared_ptr<ArrowResultCollectorSharedState> sharedState,
        ArrowResultCollectorInfo info, std::unique_ptr<PhysicalOperator> child, physical_op_id id,
        std::unique_ptr<OPPrintInfo> printInfo)
        : Sink{type_, std::move(child), id, std::move(printInfo)},
          sharedState{std::move(sharedState)}, info{std::move(info)} {}

    std::unique_ptr<main::QueryResult> getQueryResult() const override;

    void executeInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<DirectArrowResultCollector>(sharedState, info.copy(),
            children[0]->copy(), id, printInfo->copy());
    }

private:
    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext*) override;

private:
    std::shared_ptr<ArrowResultCollectorSharedState> sharedState;
    ArrowResultCollectorInfo info;
    ArrowResultCollectorLocalState localState;
};

} // namespace processor
} // namespace lbug
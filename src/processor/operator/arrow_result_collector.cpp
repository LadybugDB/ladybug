#include "processor/operator/arrow_result_collector.h"

#include <algorithm>
#include <tuple>

#include "common/arrow/arrow_row_batch.h"
#include "common/exception/runtime.h"
#include "main/query_result/arrow_query_result.h"

using namespace lbug::common;

namespace lbug {
namespace processor {

namespace {

static void updateDirectCSRMetadata(const CSRTrackingInfo& info, const std::vector<int64_t>& values,
    ArrowResultCollectorLocalState& localState) {
    if (!info.enabled() || !localState.csrMetadataValid) {
        return;
    }
    const auto srcRowID = values[info.srcRowIDColIdx];
    const auto dstRowID = values[info.dstRowIDColIdx];
    if (!localState.csrMetadata.has_value()) {
        main::ArrowQueryResult::CSRMetadata metadata;
        metadata.hasEdgeIDs = info.hasRelRowID();
        metadata.numSourceRows = info.numSourceRows;
        localState.csrMetadata = std::move(metadata);
    }
    auto& metadata = *localState.csrMetadata;
    if (srcRowID < 0 || dstRowID < 0) {
        localState.csrMetadataValid = false;
        localState.csrMetadata.reset();
        return;
    }
    // Sparse per-batch CSR build. The rel scan emits edges in non-decreasing
    // source order per thread, so we record one (srcRow, count) run per
    // distinct source row instead of a dense global indptr (which would
    // cost numSourceRows+1 entries per batch). The trailing run is flushed
    // in executeInternal(); the global dense indptr is reconstructed once
    // by kwayMergeCSRChunks() on first consumer request.
    if (localState.currentSourceRowID == -1) {
        localState.currentSourceRowID = srcRowID;
        localState.currentRowCount = 0;
    } else if (srcRowID != localState.currentSourceRowID) {
        if (srcRowID < localState.currentSourceRowID) {
            localState.csrMetadataValid = false;
            localState.csrMetadata.reset();
            return;
        }
        metadata.srcRows.push_back(localState.currentSourceRowID);
        metadata.counts.push_back(localState.currentRowCount);
        localState.currentSourceRowID = srcRowID;
        localState.currentRowCount = 0;
    }
    metadata.indices.push_back(dstRowID);
    if (info.hasRelRowID()) {
        metadata.edgeIDs.push_back(values[info.relRowIDColIdx]);
    }
    localState.currentRowCount++;
}

// Deterministic pairwise merge of two sparse CSR metadata chunks into one.
// Used only in FIXED_ORDER mode (ORDER BY / TopK on the data path), where
// per-batch chunks are collapsed to a single sorted chunk under key 0 to
// preserve global order. Materializes (src, dst, edge) entries and sorts by
// (src, edge, dst), then rebuilds the sparse (srcRows, counts) runs. The
// cheap NO_ORDER / INSERTION_ORDER path does not call this — it moves
// per-batch chunks into arraysByBatchIndex and the final k-way merge across
// batches runs lazily in ArrowQueryResult::combineCSRChunks() on first
// consumer request. FIXED_ORDER is inherently more expensive (it must sort);
// the common NO_ORDER rel-scan path takes the sparse run build instead.
static std::optional<main::ArrowQueryResult::CSRMetadata> mergeCSRMetadata(
    main::ArrowQueryResult::CSRMetadata left, main::ArrowQueryResult::CSRMetadata right) {
    if (left.hasEdgeIDs != right.hasEdgeIDs) {
        return std::nullopt;
    }
    struct CSREntry {
        int64_t src;
        int64_t dst;
        int64_t edge;
    };
    std::vector<CSREntry> entries;
    auto appendEntries = [&](const main::ArrowQueryResult::CSRMetadata& metadata) {
        if (metadata.hasEdgeIDs && metadata.edgeIDs.size() != metadata.indices.size()) {
            return false;
        }
        if (metadata.srcRows.size() != metadata.counts.size()) {
            return false;
        }
        int64_t offset = 0;
        for (auto i = 0u; i < metadata.srcRows.size(); ++i) {
            const auto src = metadata.srcRows[i];
            const auto count = metadata.counts[i];
            if (src < 0 || count < 0 ||
                static_cast<uint64_t>(offset + count) > metadata.indices.size()) {
                return false;
            }
            for (auto j = 0; j < count; ++j) {
                const auto idxAsOffset = static_cast<uint64_t>(offset + j);
                const auto edge = metadata.hasEdgeIDs ? metadata.edgeIDs[idxAsOffset] : -1;
                entries.push_back({src, metadata.indices[idxAsOffset], edge});
            }
            offset += count;
        }
        return true;
    };
    if (!appendEntries(left) || !appendEntries(right)) {
        return std::nullopt;
    }
    std::sort(entries.begin(), entries.end(), [](const CSREntry& a, const CSREntry& b) {
        return std::tie(a.src, a.edge, a.dst) < std::tie(b.src, b.edge, b.dst);
    });
    main::ArrowQueryResult::CSRMetadata merged;
    merged.hasEdgeIDs = left.hasEdgeIDs;
    merged.numSourceRows = std::max(left.numSourceRows, right.numSourceRows);
    int64_t curSrc = -1;
    int64_t curCount = 0;
    for (const auto& entry : entries) {
        if (entry.src < 0 || entry.dst < 0 || (merged.hasEdgeIDs && entry.edge < 0)) {
            return std::nullopt;
        }
        if (entry.src != curSrc) {
            if (curSrc >= 0 && curCount > 0) {
                merged.srcRows.push_back(curSrc);
                merged.counts.push_back(curCount);
            }
            curSrc = entry.src;
            curCount = 0;
        }
        merged.indices.push_back(entry.dst);
        if (merged.hasEdgeIDs) {
            merged.edgeIDs.push_back(entry.edge);
        }
        curCount++;
    }
    if (curSrc >= 0 && curCount > 0) {
        merged.srcRows.push_back(curSrc);
        merged.counts.push_back(curCount);
    }
    return merged;
}

// (The k-way CSR chunk merge that used to live here now lives next to
// ArrowQueryResult::combineCSRChunks() in arrow_query_result.cpp, so
// result construction stays zero-work. Per-batch chunks are passed
// straight through and merged lazily on first consumer request.)

} // namespace

static void updateCSRMetadata(const CSRTrackingInfo& info, FlatTuple& tuple,
    ArrowResultCollectorLocalState& localState) {
    if (!info.enabled() || !localState.csrMetadataValid) {
        return;
    }
    const auto srcRowID = tuple.getValue(info.srcRowIDColIdx)->getValue<int64_t>();
    const auto dstRowID = tuple.getValue(info.dstRowIDColIdx)->getValue<int64_t>();
    if (!localState.csrMetadata.has_value()) {
        main::ArrowQueryResult::CSRMetadata metadata;
        metadata.hasEdgeIDs = info.hasRelRowID();
        metadata.numSourceRows = info.numSourceRows;
        localState.csrMetadata = std::move(metadata);
    }
    auto& metadata = *localState.csrMetadata;
    if (srcRowID < 0 || dstRowID < 0) {
        localState.csrMetadataValid = false;
        localState.csrMetadata.reset();
        return;
    }
    // See updateDirectCSRMetadata for the sparse run rationale.
    if (localState.currentSourceRowID == -1) {
        localState.currentSourceRowID = srcRowID;
        localState.currentRowCount = 0;
    } else if (srcRowID != localState.currentSourceRowID) {
        if (srcRowID < localState.currentSourceRowID) {
            localState.csrMetadataValid = false;
            localState.csrMetadata.reset();
            return;
        }
        metadata.srcRows.push_back(localState.currentSourceRowID);
        metadata.counts.push_back(localState.currentRowCount);
        localState.currentSourceRowID = srcRowID;
        localState.currentRowCount = 0;
    }
    metadata.indices.push_back(dstRowID);
    if (info.hasRelRowID()) {
        metadata.edgeIDs.push_back(tuple.getValue(info.relRowIDColIdx)->getValue<int64_t>());
    }
    localState.currentRowCount++;
}

bool ArrowResultCollectorLocalState::advance() {
    for (int64_t i = static_cast<int64_t>(chunks.size()) - 1; i >= 0; --i) {
        chunkCursors[i]++;
        if (chunkCursors[i] < chunks[i]->state->getSelSize()) {
            return true;
        }
        chunkCursors[i] = 0;
    }
    return false;
}

void ArrowResultCollectorLocalState::fillTuple() {
    DASSERT(tuple->len() == vectors.size());
    for (auto i = 0u; i < vectors.size(); ++i) {
        auto vector = vectors[i];
        auto pos = vector->state->getSelVector()[vectorsSelPos[i]];
        auto data = vector->getData() + pos * vector->getNumBytesPerValue();
        tuple->getValue(i)->copyFromColLayout(data, vector);
    }
}

void ArrowResultCollectorLocalState::resetCursor() {
    for (auto i = 0u; i < chunkCursors.size(); ++i) {
        chunkCursors[i] = 0;
    }
}

void ArrowResultCollectorSharedState::merge(std::vector<ArrowArray> localArrays,
    batch_index_t batchIndex, std::optional<main::ArrowQueryResult::CSRMetadata> localCSRMetadata) {
    std::unique_lock lck{mutex};
    if (requireDeterministicOrder) {
        // FIXED_ORDER (ORDER BY / TopK): collapse to a single running chunk
        // under key 0 via the existing pairwise mergeCSRMetadata. This
        // preserves global sort order across threads.
        if (!localArrays.empty()) {
            auto& slot = arraysByBatchIndex[0];
            slot.insert(slot.end(), std::make_move_iterator(localArrays.begin()),
                std::make_move_iterator(localArrays.end()));
        }
        if (localCSRMetadata.has_value()) {
            auto it = csrMetadataByBatchIndex.find(0);
            if (it == csrMetadataByBatchIndex.end()) {
                csrMetadataByBatchIndex.emplace(0, std::move(*localCSRMetadata));
            } else {
                auto merged = mergeCSRMetadata(std::move(it->second), std::move(*localCSRMetadata));
                if (merged.has_value()) {
                    it->second = std::move(*merged);
                } else {
                    csrMetadataByBatchIndex.erase(it);
                }
            }
        }
        return;
    }
    // NO_ORDER / INSERTION_ORDER: cheap batch-index parallel path.
    // Per-batch chunks are moved into the global map (O(log N) per call,
    // no pairwise merge, no sort). The final k-way merge across batches
    // runs lazily in ArrowQueryResult::combineCSRChunks() on first
    // consumer request, so result construction itself does no merging
    // work for the cheap path.
    if (!localArrays.empty()) {
        auto& slot = arraysByBatchIndex[batchIndex];
        slot.insert(slot.end(), std::make_move_iterator(localArrays.begin()),
            std::make_move_iterator(localArrays.end()));
    }
    if (localCSRMetadata.has_value()) {
        csrMetadataByBatchIndex[batchIndex] = std::move(*localCSRMetadata);
    }
}

void ArrowResultCollector::executeInternal(ExecutionContext* context) {
    auto rowBatch = std::make_unique<ArrowRowBatch>(info.columnTypes, info.chunkSize,
        false /* fallbackExtensionTypes */);
    while (children[0]->getNextTuple(context)) {
        localState.resetCursor();
        while (true) {
            if (!fillRowBatch(*rowBatch)) {
                break;
            }
            localState.arrays.push_back(rowBatch->toArray(info.columnTypes));
            rowBatch = std::make_unique<ArrowRowBatch>(info.columnTypes, info.chunkSize,
                false /* fallbackExtensionTypes */);
        }
    }
    // Handle the last rowBatch whose size can be smaller than chunk size.
    if (rowBatch->size() > 0) {
        localState.arrays.push_back(rowBatch->toArray(info.columnTypes));
    }
    // Flush the trailing source row's run into the sparse (srcRows, counts)
    // representation. The global dense indptr (with trailing empty-row
    // padding) is reconstructed once by kwayMergeCSRChunks() at merge time.
    if (localState.csrMetadata.has_value() && localState.currentSourceRowID != -1) {
        auto& metadata = *localState.csrMetadata;
        metadata.srcRows.push_back(localState.currentSourceRowID);
        metadata.counts.push_back(localState.currentRowCount);
    }
    sharedState->merge(std::move(localState.arrays), localState.batchIndex,
        std::move(localState.csrMetadata));
}

bool ArrowResultCollector::fillRowBatch(ArrowRowBatch& rowBatch) {
    while (rowBatch.size() < info.chunkSize) {
        localState.fillTuple();
        updateCSRMetadata(info.csrTrackingInfo, *localState.tuple, localState);
        rowBatch.append(*localState.tuple);
        if (!localState.advance()) {
            return false;
        }
    }
    return true;
}

void ArrowResultCollector::initLocalStateInternal(ResultSet* resultSet, ExecutionContext*) {
    // Assign a unique batch_index for this local collector. The atomic
    // fetch_add inside BatchIndexAssigner is the only synchronization needed
    // to give each thread a distinct batch_index.
    localState.batchIndex = sharedState->batchIndexAssigner.next();

    std::unordered_map<idx_t, idx_t> idxMap; // Map result set chunk idx to local state idx
    // Populate chunks
    for (auto& pos : info.payloadPositions) {
        auto idx = pos.dataChunkPos;
        if (idxMap.contains(idx)) {
            continue;
        }
        idxMap.insert({idx, localState.chunks.size()});
        localState.chunks.push_back(resultSet->getDataChunk(idx).get());
        localState.chunkCursors.push_back(0);
    }
    // Populate vectors
    for (auto& pos : info.payloadPositions) {
        localState.vectors.push_back(resultSet->getValueVector(pos).get());
        localState.vectorsSelPos.push_back(localState.chunkCursors[idxMap.at(pos.dataChunkPos)]);
    }
    localState.tuple = std::make_unique<FlatTuple>(info.columnTypes);
}

std::unique_ptr<main::QueryResult> ArrowResultCollector::getQueryResult() const {
    // Walk the per-batch map in batch_index order and concatenate. The map
    // is std::map<batch_index_t, ...>, so iteration is naturally ordered.
    std::vector<ArrowArray> arrays;
    for (auto& [batchIdx, batchArrays] : sharedState->arraysByBatchIndex) {
        for (auto& arr : batchArrays) {
            arrays.push_back(std::move(arr));
        }
    }
    std::vector<main::ArrowQueryResult::CSRMetadata> csrChunks;
    csrChunks.reserve(sharedState->csrMetadataByBatchIndex.size());
    for (auto& [batchIdx, csr] : sharedState->csrMetadataByBatchIndex) {
        csrChunks.push_back(std::move(csr));
    }
    return std::make_unique<main::ArrowQueryResult>(std::move(arrays), info.chunkSize,
        std::move(csrChunks));
}

void DirectArrowResultCollector::initLocalStateInternal(ResultSet* resultSet, ExecutionContext*) {
    localState.batchIndex = sharedState->batchIndexAssigner.next();

    std::unordered_map<idx_t, idx_t> idxMap;
    for (auto& pos : info.payloadPositions) {
        auto idx = pos.dataChunkPos;
        if (idxMap.contains(idx)) {
            continue;
        }
        idxMap.insert({idx, localState.chunks.size()});
        localState.chunks.push_back(resultSet->getDataChunk(idx).get());
        localState.chunkCursors.push_back(0);
    }
    for (auto& pos : info.payloadPositions) {
        auto vector = resultSet->getValueVector(pos).get();
        if (vector->dataType.getLogicalTypeID() != LogicalTypeID::INT64) {
            throw RuntimeException(
                "Direct Arrow CSR collector only supports INT64 rowid projections.");
        }
        localState.vectors.push_back(vector);
        localState.vectorsSelPos.push_back(localState.chunkCursors[idxMap.at(pos.dataChunkPos)]);
    }
}

void DirectArrowResultCollector::executeInternal(ExecutionContext* context) {
    // The Direct collector only sees INT64 rowid projections on a
    // CSR-shaped query. The (src, edge, dst) tuples are already being
    // captured into localState.csrMetadata by updateDirectCSRMetadata, so
    // duplicating them into per-column vectors and re-wrapping them as
    // ArrowArrays is pure waste: every rowid that lands in the ArrowArrays
    // also lands in csrMetadata->indices / edgeIDs. For a 1B-edge query
    // with 3 INT64 columns at chunkSize=1M that double materialization
    // is ~24GB of ArrowArrays on top of the ~24GB of CSR chunks, and is
    // the dominant resident-set cost of the .csr() flow (the consumer
    // never touches the ArrowArrays — they're already in CSR).
    //
    // Skip the columns/flushChunk path entirely. localState.arrays stays
    // empty; sharedState->merge sees no ArrowArrays to store, so
    // arraysByBatchIndex for this batch_index is empty and the
    // ArrowQueryResult's ArrowChunkedArray is empty for this collector.
    // The CSR side is unaffected: it gets built and combined as before.
    std::vector<int64_t> rowValues(info.payloadPositions.size());

    while (children[0]->getNextTuple(context)) {
        localState.resetCursor();
        while (true) {
            for (auto i = 0u; i < localState.vectors.size(); ++i) {
                auto vector = localState.vectors[i];
                auto pos = vector->state->getSelVector()[localState.vectorsSelPos[i]];
                if (vector->isNull(pos)) {
                    throw RuntimeException(
                        "Direct Arrow CSR collector cannot export null rowid values.");
                }
                rowValues[i] = vector->getValue<int64_t>(pos);
            }
            updateDirectCSRMetadata(info.csrTrackingInfo, rowValues, localState);
            if (!localState.advance()) {
                break;
            }
        }
    }
    // Flush the trailing source row's run into the sparse (srcRows, counts)
    // representation. The global dense indptr (with trailing empty-row
    // padding) is reconstructed once by kwayMergeCSRChunks() at merge time.
    if (localState.csrMetadata.has_value() && localState.currentSourceRowID != -1) {
        auto& metadata = *localState.csrMetadata;
        metadata.srcRows.push_back(localState.currentSourceRowID);
        metadata.counts.push_back(localState.currentRowCount);
    }
    sharedState->merge({}, localState.batchIndex, std::move(localState.csrMetadata));
}

std::unique_ptr<main::QueryResult> DirectArrowResultCollector::getQueryResult() const {
    std::vector<ArrowArray> arrays;
    for (auto& [batchIdx, batchArrays] : sharedState->arraysByBatchIndex) {
        for (auto& arr : batchArrays) {
            arrays.push_back(std::move(arr));
        }
    }
    std::vector<main::ArrowQueryResult::CSRMetadata> csrChunks;
    csrChunks.reserve(sharedState->csrMetadataByBatchIndex.size());
    for (auto& [batchIdx, csr] : sharedState->csrMetadataByBatchIndex) {
        csrChunks.push_back(std::move(csr));
    }
    return std::make_unique<main::ArrowQueryResult>(std::move(arrays), info.chunkSize,
        std::move(csrChunks));
}

} // namespace processor
} // namespace lbug
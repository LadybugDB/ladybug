#include "processor/operator/arrow_result_collector.h"

#include <algorithm>
#include <array>
#include <tuple>

#include "common/arrow/arrow_row_batch.h"
#include "common/exception/runtime.h"
#include "main/query_result/arrow_query_result.h"

using namespace lbug::common;

namespace lbug {
namespace processor {

namespace {

struct DirectArrowChunkHolder {
    std::vector<std::vector<int64_t>> columns;
    std::array<const void*, 1> rootBuffers = {{nullptr}};
    std::vector<std::array<const void*, 2>> childBuffers;
    std::vector<ArrowArray> children;
    std::vector<ArrowArray*> childPtrs;
};

static void releaseDirectArrowChunk(ArrowArray* array) {
    if (!array || !array->release) {
        return;
    }
    array->release = nullptr;
    auto holder = static_cast<DirectArrowChunkHolder*>(array->private_data);
    delete holder;
    array->private_data = nullptr;
}

static ArrowArray makeDirectInt64ArrowChunk(std::vector<std::vector<int64_t>> columns) {
    auto holder = std::make_unique<DirectArrowChunkHolder>();
    holder->columns = std::move(columns);
    const auto numColumns = holder->columns.size();
    const auto length = numColumns == 0 ? 0 : holder->columns[0].size();
    holder->childBuffers.resize(numColumns);
    holder->children.resize(numColumns);
    holder->childPtrs.resize(numColumns);
    for (auto i = 0u; i < numColumns; ++i) {
        holder->childBuffers[i][0] = nullptr;
        holder->childBuffers[i][1] = holder->columns[i].data();
        auto& child = holder->children[i];
        child.length = static_cast<int64_t>(holder->columns[i].size());
        child.null_count = 0;
        child.offset = 0;
        child.n_buffers = 2;
        child.n_children = 0;
        child.buffers = holder->childBuffers[i].data();
        child.children = nullptr;
        child.dictionary = nullptr;
        child.release = nullptr;
        child.private_data = nullptr;
        holder->childPtrs[i] = &child;
    }

    ArrowArray result{};
    result.length = static_cast<int64_t>(length);
    result.null_count = 0;
    result.offset = 0;
    result.n_buffers = 1;
    result.n_children = static_cast<int64_t>(numColumns);
    result.buffers = holder->rootBuffers.data();
    result.children = holder->childPtrs.data();
    result.dictionary = nullptr;
    result.private_data = holder.release();
    result.release = releaseDirectArrowChunk;
    return result;
}

static void updateDirectCSRMetadata(const CSRTrackingInfo& info, const std::vector<int64_t>& values,
    ArrowResultCollectorLocalState& localState) {
    if (!info.enabled() || !localState.csrMetadataValid) {
        return;
    }
    const auto srcRowID = values[info.srcRowIDColIdx];
    const auto dstRowID = values[info.dstRowIDColIdx];
    if (!localState.csrMetadata.has_value()) {
        main::ArrowQueryResult::CSRMetadata metadata;
        metadata.indptr.push_back(0);
        metadata.hasEdgeIDs = info.hasRelRowID();
        localState.csrMetadata = std::move(metadata);
    }
    auto& metadata = *localState.csrMetadata;
    if (srcRowID < 0 || dstRowID < 0) {
        localState.csrMetadataValid = false;
        localState.csrMetadata.reset();
        return;
    }
    if (localState.currentSourceRowID == -1) {
        while (localState.nextSourceRowID < srcRowID) {
            metadata.indptr.push_back(static_cast<int64_t>(metadata.indices.size()));
            localState.nextSourceRowID++;
        }
        localState.currentSourceRowID = srcRowID;
    } else if (srcRowID != localState.currentSourceRowID) {
        if (srcRowID < localState.currentSourceRowID) {
            localState.csrMetadataValid = false;
            localState.csrMetadata.reset();
            return;
        }
        metadata.indptr.push_back(static_cast<int64_t>(metadata.indices.size()));
        localState.nextSourceRowID = localState.currentSourceRowID + 1;
        while (localState.nextSourceRowID < srcRowID) {
            metadata.indptr.push_back(static_cast<int64_t>(metadata.indices.size()));
            localState.nextSourceRowID++;
        }
        localState.currentSourceRowID = srcRowID;
    }
    metadata.indices.push_back(dstRowID);
    if (info.hasRelRowID()) {
        metadata.edgeIDs.push_back(values[info.relRowIDColIdx]);
    }
}

static std::optional<main::ArrowQueryResult::CSRMetadata> mergeCSRMetadata(
    const main::ArrowQueryResult::CSRMetadata& left,
    const main::ArrowQueryResult::CSRMetadata& right) {
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
        if (metadata.indptr.empty()) {
            return true;
        }
        for (auto src = 0u; src + 1 < metadata.indptr.size(); ++src) {
            const auto begin = metadata.indptr[src];
            const auto end = metadata.indptr[src + 1];
            if (begin < 0 || end < begin || static_cast<uint64_t>(end) > metadata.indices.size()) {
                return false;
            }
            for (auto idx = begin; idx < end; ++idx) {
                const auto idxAsOffset = static_cast<uint64_t>(idx);
                const auto edge = metadata.hasEdgeIDs ? metadata.edgeIDs[idxAsOffset] : -1;
                entries.push_back({static_cast<int64_t>(src), metadata.indices[idxAsOffset], edge});
            }
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
    merged.indptr.push_back(0);
    int64_t nextSourceRowID = 0;
    for (const auto& entry : entries) {
        if (entry.src < 0 || entry.dst < 0 || (merged.hasEdgeIDs && entry.edge < 0)) {
            return std::nullopt;
        }
        while (nextSourceRowID < entry.src) {
            merged.indptr.push_back(static_cast<int64_t>(merged.indices.size()));
            nextSourceRowID++;
        }
        merged.indices.push_back(entry.dst);
        if (merged.hasEdgeIDs) {
            merged.edgeIDs.push_back(entry.edge);
        }
    }
    merged.indptr.push_back(static_cast<int64_t>(merged.indices.size()));
    return merged;
}

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
        metadata.indptr.push_back(0);
        metadata.hasEdgeIDs = info.hasRelRowID();
        localState.csrMetadata = std::move(metadata);
    }
    auto& metadata = *localState.csrMetadata;
    if (srcRowID < 0 || dstRowID < 0) {
        localState.csrMetadataValid = false;
        localState.csrMetadata.reset();
        return;
    }
    if (localState.currentSourceRowID == -1) {
        while (localState.nextSourceRowID < srcRowID) {
            metadata.indptr.push_back(static_cast<int64_t>(metadata.indices.size()));
            localState.nextSourceRowID++;
        }
        localState.currentSourceRowID = srcRowID;
    } else if (srcRowID != localState.currentSourceRowID) {
        if (srcRowID < localState.currentSourceRowID) {
            localState.csrMetadataValid = false;
            localState.csrMetadata.reset();
            return;
        }
        metadata.indptr.push_back(static_cast<int64_t>(metadata.indices.size()));
        localState.nextSourceRowID = localState.currentSourceRowID + 1;
        while (localState.nextSourceRowID < srcRowID) {
            metadata.indptr.push_back(static_cast<int64_t>(metadata.indices.size()));
            localState.nextSourceRowID++;
        }
        localState.currentSourceRowID = srcRowID;
    }
    metadata.indices.push_back(dstRowID);
    if (info.hasRelRowID()) {
        metadata.edgeIDs.push_back(tuple.getValue(info.relRowIDColIdx)->getValue<int64_t>());
    }
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

void ArrowResultCollectorSharedState::merge(const std::vector<ArrowArray>& localArrays,
    const std::optional<main::ArrowQueryResult::CSRMetadata>& localCSRMetadata) {
    std::unique_lock lck{mutex};
    for (auto i = 0u; i < localArrays.size(); ++i) {
        arrays.push_back(localArrays[i]);
    }
    if (!csrMetadata.has_value() && localCSRMetadata.has_value()) {
        csrMetadata = localCSRMetadata;
    } else if (csrMetadata.has_value() && localCSRMetadata.has_value()) {
        csrMetadata = mergeCSRMetadata(*csrMetadata, *localCSRMetadata);
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
    if (localState.csrMetadata.has_value()) {
        localState.csrMetadata->indptr.push_back(
            static_cast<int64_t>(localState.csrMetadata->indices.size()));
    }
    sharedState->merge(localState.arrays, localState.csrMetadata);
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
    if (sharedState->csrMetadata.has_value()) {
        return std::make_unique<main::ArrowQueryResult>(std::move(sharedState->arrays),
            info.chunkSize, std::move(*sharedState->csrMetadata));
    }
    return std::make_unique<main::ArrowQueryResult>(std::move(sharedState->arrays), info.chunkSize);
}

void DirectArrowResultCollector::initLocalStateInternal(ResultSet* resultSet, ExecutionContext*) {
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
    std::vector<std::vector<int64_t>> columns(info.payloadPositions.size());
    std::vector<int64_t> rowValues(info.payloadPositions.size());
    auto flushChunk = [&]() {
        if (columns.empty() || columns[0].empty()) {
            return;
        }
        localState.arrays.push_back(makeDirectInt64ArrowChunk(std::move(columns)));
        columns = std::vector<std::vector<int64_t>>(info.payloadPositions.size());
    };

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
                columns[i].push_back(rowValues[i]);
            }
            updateDirectCSRMetadata(info.csrTrackingInfo, rowValues, localState);
            if (columns[0].size() == static_cast<uint64_t>(info.chunkSize)) {
                flushChunk();
            }
            if (!localState.advance()) {
                break;
            }
        }
    }
    flushChunk();
    if (localState.csrMetadata.has_value()) {
        localState.csrMetadata->indptr.push_back(
            static_cast<int64_t>(localState.csrMetadata->indices.size()));
    }
    sharedState->merge(localState.arrays, localState.csrMetadata);
}

std::unique_ptr<main::QueryResult> DirectArrowResultCollector::getQueryResult() const {
    if (sharedState->csrMetadata.has_value()) {
        return std::make_unique<main::ArrowQueryResult>(std::move(sharedState->arrays),
            info.chunkSize, std::move(*sharedState->csrMetadata));
    }
    return std::make_unique<main::ArrowQueryResult>(std::move(sharedState->arrays), info.chunkSize);
}

} // namespace processor
} // namespace lbug

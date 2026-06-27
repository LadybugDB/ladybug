#include "main/query_result/arrow_query_result.h"

#include <array>

#include "common/arrow/arrow_row_batch.h"
#include "common/exception/not_implemented.h"
#include "common/exception/runtime.h"
#include "processor/result/factorized_table.h"
#include <format>

using namespace lbug::common;
using namespace lbug::processor;

namespace lbug {
namespace main {

namespace {

struct CSRArrowArrayHolder {
    std::shared_ptr<const std::vector<int64_t>> values;
    std::array<const void*, 2> buffers = {{nullptr, nullptr}};
};

static void releaseCSRArrowArray(ArrowArray* array) {
    if (!array || !array->release) {
        return;
    }
    array->release = nullptr;
    auto holder = static_cast<CSRArrowArrayHolder*>(array->private_data);
    delete holder;
    array->private_data = nullptr;
}

static void releaseCSRArrowSchema(ArrowSchema* schema) {
    if (!schema || !schema->release) {
        return;
    }
    schema->release = nullptr;
}

static ArrowQueryResult::CSRArrowArray makeCSRArrowArray(
    std::shared_ptr<const std::vector<int64_t>> values) {
    ArrowQueryResult::CSRArrowArray result;

    auto holder = std::make_unique<CSRArrowArrayHolder>();
    holder->values = std::move(values);
    holder->buffers[0] = nullptr;
    holder->buffers[1] = holder->values->data();

    result.array.length = static_cast<int64_t>(holder->values->size());
    result.array.null_count = 0;
    result.array.offset = 0;
    result.array.n_buffers = 2;
    result.array.n_children = 0;
    result.array.buffers = holder->buffers.data();
    result.array.children = nullptr;
    result.array.dictionary = nullptr;
    result.array.private_data = holder.release();
    result.array.release = releaseCSRArrowArray;

    result.schema.format = "l";
    result.schema.name = nullptr;
    result.schema.metadata = nullptr;
    result.schema.flags = 0;
    result.schema.n_children = 0;
    result.schema.children = nullptr;
    result.schema.dictionary = nullptr;
    result.schema.private_data = nullptr;
    result.schema.release = releaseCSRArrowSchema;

    return result;
}

// K-way merge of per-batch CSR metadata chunks (in batch_index order)
// into a single flat CSRMetadata. Each chunk's indptr is indexed by
// GLOBAL source row id: the per-batch CSR tracker fills indptr densely
// from global row 0 up to that chunk's max source row, with a trailing
// sentinel equal to indices.size(). For each global source row in
// ascending order, emit edges from each chunk in batch_index order;
// within a chunk the per-batch tracker already groups entries by
// source row in scan order, so emitting in (src, batch_index,
// scan_order_within_batch) order is correct without any global sort.
//
// Runs lazily on first consumer request via combineCSRChunks(), not at
// result-construction time, so result construction stays zero-work for
// the NO_ORDER / INSERTION_ORDER path.
static ArrowQueryResult::CSRMetadata kwayMergeCSRChunks(
    std::vector<ArrowQueryResult::CSRMetadata> chunks) {
    if (chunks.empty()) {
        return ArrowQueryResult::CSRMetadata{};
    }
    if (chunks.size() == 1) {
        return std::move(chunks[0]);
    }
    const auto& first = chunks.front();
    const auto hasEdgeIDs = first.hasEdgeIDs;

    int64_t maxSrcRow = 0;
    size_t totalIndices = 0;
    int64_t numSourceRows = 0;
    bool sawChunk = false;
    for (const auto& c : chunks) {
        if (c.hasEdgeIDs != hasEdgeIDs) {
            return ArrowQueryResult::CSRMetadata{};
        }
        if (c.hasEdgeIDs && c.edgeIDs.size() != c.indices.size()) {
            return ArrowQueryResult::CSRMetadata{};
        }
        if (c.numSourceRows > numSourceRows) {
            numSourceRows = c.numSourceRows;
        }
        if (c.indptr.size() < 2) {
            continue;
        }
        if (c.indptr.back() != static_cast<int64_t>(c.indices.size())) {
            return ArrowQueryResult::CSRMetadata{};
        }
        const auto numSrcRows = static_cast<int64_t>(c.indptr.size() - 1);
        if (numSrcRows > maxSrcRow) {
            maxSrcRow = numSrcRows;
        }
        totalIndices += c.indices.size();
        sawChunk = true;
    }
    ArrowQueryResult::CSRMetadata merged;
    merged.hasEdgeIDs = hasEdgeIDs;
    merged.numSourceRows = numSourceRows;
    merged.indptr.push_back(0);
    if (!sawChunk || maxSrcRow == 0) {
        // Pad to numSourceRows + 1 entries even when there are no edges.
        for (int64_t i = 0; i < numSourceRows; ++i) {
            merged.indptr.push_back(0);
        }
        return merged;
    }
    merged.indices.reserve(totalIndices);
    if (hasEdgeIDs) {
        merged.edgeIDs.reserve(totalIndices);
    }
    for (int64_t src = 0; src < maxSrcRow; ++src) {
        for (const auto& c : chunks) {
            if (static_cast<size_t>(src + 1) >= c.indptr.size()) {
                continue;
            }
            const auto begin = c.indptr[src];
            const auto end = c.indptr[src + 1];
            if (begin < 0 || end < begin || static_cast<uint64_t>(end) > c.indices.size()) {
                return ArrowQueryResult::CSRMetadata{};
            }
            for (auto idx = begin; idx < end; ++idx) {
                const auto u = static_cast<uint64_t>(idx);
                merged.indices.push_back(c.indices[u]);
                if (hasEdgeIDs) {
                    merged.edgeIDs.push_back(c.edgeIDs[u]);
                }
            }
        }
        merged.indptr.push_back(static_cast<int64_t>(merged.indices.size()));
    }
    // Fill trailing empty source rows (nodes after the last source row
    // with edges in any chunk). maxSrcRow is the highest numSrcRows
    // (indptr.size() - 1) across all chunks; if it is less than
    // numSourceRows, the trailing node rows have zero edges and need
    // empty entries in the merged indptr so the CSR is indexable up
    // to numSourceRows - 1.
    for (int64_t src = maxSrcRow; src < numSourceRows; ++src) {
        merged.indptr.push_back(static_cast<int64_t>(merged.indices.size()));
    }
    return merged;
}

} // namespace

ArrowQueryResult::ArrowQueryResult(std::vector<ArrowArray> arrays, int64_t chunkSize)
    : QueryResult{type_},
      arraysStorage{std::make_shared<std::vector<ArrowArray>>(std::move(arrays))},
      chunkSize_{chunkSize} {
    for (const auto& array : *arraysStorage) {
        numTuples += array.length;
    }
}

ArrowQueryResult::ArrowQueryResult(std::vector<ArrowArray> arrays, int64_t chunkSize,
    std::vector<CSRMetadata> csrChunks)
    : QueryResult{type_},
      arraysStorage{std::make_shared<std::vector<ArrowArray>>(std::move(arrays))},
      chunkSize_{chunkSize}, csrChunks{std::move(csrChunks)} {
    for (const auto& array : *arraysStorage) {
        numTuples += array.length;
    }
}

ArrowQueryResult::ArrowQueryResult(std::vector<std::string> columnNames,
    std::vector<LogicalType> columnTypes, FactorizedTable& table, int64_t chunkSize)
    : QueryResult{type_, std::move(columnNames), std::move(columnTypes)},
      arraysStorage{std::make_shared<std::vector<ArrowArray>>()}, chunkSize_{chunkSize} {
    auto iterator = FactorizedTableIterator(table);
    while (iterator.hasNext()) {
        arraysStorage->push_back(getArray(iterator, chunkSize));
    }
}

uint64_t ArrowQueryResult::getNumTuples() const {
    return numTuples;
}

ArrowArray ArrowQueryResult::getArray(FactorizedTableIterator& iterator, int64_t chunkSize) {
    auto rowBatch = ArrowRowBatch(columnTypes, chunkSize, false /* fallbackExtensionTypes */);
    auto rowBatchSize = 0u;
    while (rowBatchSize < chunkSize) {
        if (!iterator.hasNext()) {
            break;
        }
        (void)iterator.getNext(*tuple);
        rowBatch.append(*tuple);
        rowBatchSize++;
        numTuples++;
    }
    return rowBatch.toArray(columnTypes);
}

bool ArrowQueryResult::hasNext() const {
    throw NotImplementedException(
        "ArrowQueryResult does not implement hasNext. Use MaterializedQueryResult instead.");
}

std::shared_ptr<FlatTuple> ArrowQueryResult::getNext() {
    throw NotImplementedException(
        "ArrowQueryResult does not implement getNext. Use MaterializedQueryResult instead.");
}

void ArrowQueryResult::resetIterator() {
    cursor = 0u;
}

std::string ArrowQueryResult::toString() const {
    throw NotImplementedException(
        "ArrowQueryResult does not implement toString. Use MaterializedQueryResult instead.");
}

bool ArrowQueryResult::hasNextArrowChunk() {
    return cursor < arraysStorage->size();
}

std::unique_ptr<ArrowArray> ArrowQueryResult::getNextArrowChunk(int64_t chunkSize) {
    if (chunkSize != chunkSize_) {
        throw RuntimeException(
            std::format("Chunk size does not match expected value {}.", chunkSize_));
    }
    return std::make_unique<ArrowArray>((*arraysStorage)[cursor++]);
}

ArrowQueryResult::ArrowChunkedArray ArrowQueryResult::getArrowChunks() const {
    // Implicit conversion from shared_ptr<vector<ArrowArray>> to
    // shared_ptr<const vector<ArrowArray>>.
    return ArrowChunkedArray{arraysStorage};
}

ArrowQueryResult::CSRArrowArrays ArrowQueryResult::getCSRArrowArrays() const {
    if (!hasCSRMetadata()) {
        throw RuntimeException("Arrow query result does not have CSR metadata.");
    }
    const CSRMetadata& merged = combineCSRChunks();
    CSRArrowArrays result;
    result.indptr =
        makeCSRArrowArray(std::shared_ptr<const std::vector<int64_t>>(combinedCSR, &merged.indptr));
    result.indices = makeCSRArrowArray(
        std::shared_ptr<const std::vector<int64_t>>(combinedCSR, &merged.indices));
    if (merged.hasEdgeIDs) {
        result.edgeIDs = makeCSRArrowArray(
            std::shared_ptr<const std::vector<int64_t>>(combinedCSR, &merged.edgeIDs));
    }
    return result;
}

const ArrowQueryResult::CSRMetadata& ArrowQueryResult::combineCSRChunks() const {
    if (combinedCSR) {
        return *combinedCSR;
    }
    combinedCSR = std::make_shared<const CSRMetadata>(kwayMergeCSRChunks(csrChunks));
    return *combinedCSR;
}

} // namespace main
} // namespace lbug

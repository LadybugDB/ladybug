#include "main/query_result/arrow_query_result.h"

#include <array>
#include <queue>

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

// Build a dense global indptr of size numSourceRows+1 from sparse
// (srcRows, counts) runs. indptr[src+1] is set to counts[i] for the
// touched src = srcRows[i], then prefix-summed so indptr[src] gives the
// offset of src's edges in the (source-sorted) indices vector. Source
// rows absent from srcRows keep their slot at 0, i.e. no edges. Returns
// an empty vector on validation failure (bad src/count, or the
// disjointness invariant is violated — a source row appearing more than
// once would silently corrupt the merged CSR).
static std::vector<int64_t> buildDenseIndptr(int64_t numSourceRows,
    const std::vector<int64_t>& srcRows, const std::vector<int64_t>& counts) {
    std::vector<int64_t> indptr(static_cast<size_t>(numSourceRows) + 1, 0);
    for (auto i = 0u; i < srcRows.size(); ++i) {
        const auto src = srcRows[i];
        const auto count = counts[i];
        if (src < 0 || src >= numSourceRows || count < 0) {
            return {};
        }
        if (indptr[static_cast<size_t>(src) + 1] != 0) {
            return {};
        }
        indptr[static_cast<size_t>(src) + 1] = count;
    }
    for (size_t i = 0; i + 1 < indptr.size(); ++i) {
        indptr[i + 1] += indptr[i];
    }
    return indptr;
}

// K-way merge of per-batch sparse CSR metadata chunks (in batch_index
// order) into a single flat CSRMetadata with a dense global indptr.
// Per-batch chunks carry (srcRows, counts) sparse runs — NOT a dense
// global indptr — so a batch only pays for the distinct source rows it
// touched (the old dense representation cost numSourceRows+1 entries per
// batch, i.e. ~B x 800MB for a 100M-node table, which was the 45GB
// blow-up). The rel scan emits edges in non-decreasing source order per
// thread (CSR storage + monotonic morsel acquisition), and each source
// node is scanned in exactly one morsel -> one thread, so per-batch
// srcRows sets are disjoint and sorted: this is a union of sorted
// disjoint runs, not a general k-way merge.
//
// Single-chunk fast path: the chunk's indices are already laid out in
// source-row order, so we move indices/edgeIDs through without copying
// and synthesize the dense indptr from (srcRows, counts).
//
// Multi-chunk path: k-way merge by source row via a min-heap, copying
// edges into merged.indices in global source order, then prefix-sum the
// per-source-row counts into the dense indptr.
//
// Runs lazily on first consumer request via combineCSRChunks(), not at
// result-construction time, so result construction stays zero-work for
// the NO_ORDER / INSERTION_ORDER path.
static ArrowQueryResult::CSRMetadata kwayMergeCSRChunks(
    std::vector<ArrowQueryResult::CSRMetadata> chunks) {
    if (chunks.empty()) {
        return ArrowQueryResult::CSRMetadata{};
    }
    const auto& first = chunks.front();
    const auto hasEdgeIDs = first.hasEdgeIDs;

    int64_t numSourceRows = 0;
    size_t totalIndices = 0;
    for (const auto& c : chunks) {
        if (c.hasEdgeIDs != hasEdgeIDs) {
            return ArrowQueryResult::CSRMetadata{};
        }
        if (c.hasEdgeIDs && c.edgeIDs.size() != c.indices.size()) {
            return ArrowQueryResult::CSRMetadata{};
        }
        if (c.srcRows.size() != c.counts.size()) {
            return ArrowQueryResult::CSRMetadata{};
        }
        int64_t sumCounts = 0;
        for (auto cnt : c.counts) {
            if (cnt < 0) {
                return ArrowQueryResult::CSRMetadata{};
            }
            sumCounts += cnt;
        }
        if (static_cast<size_t>(sumCounts) != c.indices.size()) {
            return ArrowQueryResult::CSRMetadata{};
        }
        if (c.numSourceRows > numSourceRows) {
            numSourceRows = c.numSourceRows;
        }
        totalIndices += c.indices.size();
    }

    ArrowQueryResult::CSRMetadata merged;
    merged.hasEdgeIDs = hasEdgeIDs;
    merged.numSourceRows = numSourceRows;

    if (chunks.size() == 1) {
        auto& c = chunks[0];
        merged.indices = std::move(c.indices);
        merged.edgeIDs = std::move(c.edgeIDs);
        merged.indptr = buildDenseIndptr(numSourceRows, c.srcRows, c.counts);
        if (merged.indptr.empty()) {
            return ArrowQueryResult::CSRMetadata{};
        }
        return merged;
    }

    merged.indices.reserve(totalIndices);
    if (hasEdgeIDs) {
        merged.edgeIDs.reserve(totalIndices);
    }
    merged.indptr.assign(static_cast<size_t>(numSourceRows) + 1, 0);

    struct HeapItem {
        int64_t src;
        size_t chunk;
        size_t cursor;
    };
    auto cmp = [](const HeapItem& a, const HeapItem& b) { return a.src > b.src; };
    std::priority_queue<HeapItem, std::vector<HeapItem>, decltype(cmp)> heap(cmp);
    std::vector<int64_t> chunkLocalOff(chunks.size(), 0);
    for (size_t ci = 0; ci < chunks.size(); ++ci) {
        if (!chunks[ci].srcRows.empty()) {
            heap.push({chunks[ci].srcRows[0], ci, 0});
        }
    }
    while (!heap.empty()) {
        const auto top = heap.top();
        heap.pop();
        const auto src = top.src;
        auto& c = chunks[top.chunk];
        const auto count = c.counts[top.cursor];
        const int64_t localOff = chunkLocalOff[top.chunk];
        if (src < 0 || src >= numSourceRows || count < 0 ||
            static_cast<uint64_t>(localOff + count) > c.indices.size()) {
            return ArrowQueryResult::CSRMetadata{};
        }
        if (merged.indptr[static_cast<size_t>(src) + 1] != 0) {
            return ArrowQueryResult::CSRMetadata{};
        }
        for (int64_t j = 0; j < count; ++j) {
            const auto u = static_cast<uint64_t>(localOff + j);
            merged.indices.push_back(c.indices[u]);
            if (hasEdgeIDs) {
                merged.edgeIDs.push_back(c.edgeIDs[u]);
            }
        }
        chunkLocalOff[top.chunk] += count;
        merged.indptr[static_cast<size_t>(src) + 1] = count;
        const size_t next = top.cursor + 1;
        if (next < c.srcRows.size()) {
            heap.push({c.srcRows[next], top.chunk, next});
        }
    }
    for (size_t i = 0; i + 1 < merged.indptr.size(); ++i) {
        merged.indptr[i + 1] += merged.indptr[i];
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

#include "main/query_result/arrow_query_result.h"

#include <algorithm>
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
    // All chunks come from the same rel scan, so they share sortedByDest.
    // Propagate it through the merge so symmetrize() can take the fast path.
    const auto sortedByDest = first.sortedByDest;

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
        // Sparse-run form: sum(counts) must equal indices.size(). Legacy
        // dense form (empty srcRows, pre-set indptr): skip this check,
        // there are no per-source counts to sum.
        if (!c.srcRows.empty() && static_cast<size_t>(sumCounts) != c.indices.size()) {
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
    merged.sortedByDest = sortedByDest;

    if (chunks.size() == 1) {
        auto& c = chunks[0];
        merged.indices = std::move(c.indices);
        merged.edgeIDs = std::move(c.edgeIDs);
        // Sparse-run form (production path): rebuild the dense indptr from
        // (srcRows, counts). Legacy dense form (test fixtures / older
        // callers that set indptr directly with empty srcRows): keep the
        // provided indptr as-is.
        if (!c.srcRows.empty()) {
            merged.indptr = buildDenseIndptr(numSourceRows, c.srcRows, c.counts);
            if (merged.indptr.empty()) {
                return ArrowQueryResult::CSRMetadata{};
            }
        } else {
            merged.indptr = std::move(c.indptr);
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
        } else {
            // This chunk's source rows are all consumed and its edges have
            // been copied into the merged arrays. Free its vectors now so the
            // merge doesn't hold the full per-batch set + the merged copy at
            // once. Chunks whose max source row is low are freed early; those
            // spanning near numSourceRows are freed late. This is safe: a
            // chunk with an exhausted cursor is never pushed again, and the
            // heap loop only touches c when popping a live cursor entry.
            std::vector<int64_t>().swap(c.indices);
            std::vector<int64_t>().swap(c.edgeIDs);
            std::vector<int64_t>().swap(c.srcRows);
            std::vector<int64_t>().swap(c.counts);
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
    result.sortedByDest = merged.sortedByDest;
    return result;
}

const ArrowQueryResult::CSRMetadata& ArrowQueryResult::combineCSRChunks() const {
    if (combinedCSR) {
        return *combinedCSR;
    }
    // std::move the per-batch chunks into the merge so that (a) we don't
    // copy ~6GB of per-batch indices/edgeIDs into the function argument,
    // and (b) kwayMergeCSRChunks can free each chunk's vectors as soon as
    // its source rows are consumed, keeping the transient peak (per-batch
    // + merged) low instead of holding both for the whole merge. csrChunks
    // is left empty; hasCSRMetadata() stays true via combinedCSR.
    combinedCSR = std::make_shared<const CSRMetadata>(kwayMergeCSRChunks(std::move(csrChunks)));
    return *combinedCSR;
}

ArrowQueryResult::CSRArrowArrays ArrowQueryResult::CSRArrowArrays::symmetrize() const {
    const auto n = static_cast<int64_t>(indptr.array.length - 1);
    const auto m = static_cast<int64_t>(indices.array.length);
    const auto* indptrData = static_cast<const int64_t*>(indptr.array.buffers[1]);
    const auto* indicesData = static_cast<const int64_t*>(indices.array.buffers[1]);

    // Step 1: build Aᵀ (CSR) in O(n + m) via counting + scatter. For
    // each (i, j) ∈ A, increment atIndptr[j + 1] (in-degree of j),
    // prefix-sum into atIndptr, then scatter i into Aᵀ's row j at the
    // cursor. Because the scatter iterates source rows i in increasing
    // order, Aᵀ's neighbors for each row end up sorted in increasing
    // source-row order, which is what lets the per-row merge below be
    // a two-pointer walk instead of a per-row sort.
    std::vector<int64_t> atIndptr(static_cast<size_t>(n) + 1);
    for (int64_t k = 0; k < m; ++k) {
        ++atIndptr[indicesData[k] + 1];
    }
    for (int64_t j = 1; j <= n; ++j) {
        atIndptr[j] += atIndptr[j - 1];
    }
    std::vector<int64_t> cursor = atIndptr;
    std::vector<int64_t> atTIndices(static_cast<size_t>(m));
    for (int64_t i = 0; i < n; ++i) {
        const auto begin = indptrData[i];
        const auto end = indptrData[i + 1];
        for (auto k = begin; k < end; ++k) {
            atTIndices[cursor[indicesData[k]]++] = i;
        }
    }

    // Step 2: provide a sorted view of A's neighbors per row for the
    // two-pointer merge. When sortedByDest is set (the rel table was
    // declared ALTER TABLE ... SET SORTED BY (FROM ASC, TO ASC) CSR and
    // not mutated since), A's neighbors are already
    // non-decreasing within each row, so we read the raw indices
    // directly — no copy, no per-row sort, O(m) total. Otherwise the
    // CSR metadata's neighbors are populated in rel-scan order (see
    // updateDirectCSRMetadata / updateCSRMetadata), not necessarily
    // non-decreasing within a row, so we copy into a writable buffer
    // and sort each row in place. Total work for the slow path is
    // Σ dᵢ log dᵢ ≤ m log max(dᵢ).
    std::vector<int64_t> aIndicesStorage;
    const int64_t* aIndices = nullptr;
    if (sortedByDest) {
        // Fast path: read directly from the backing buffer. Safe because
        // symmetrize() never mutates A's indices — the two-pointer merge
        // only advances read cursors.
        aIndices = indicesData;
    } else {
        aIndicesStorage.resize(static_cast<size_t>(m));
        for (int64_t i = 0; i < n; ++i) {
            const auto begin = indptrData[i];
            const auto end = indptrData[i + 1];
            std::copy(indicesData + begin, indicesData + end, aIndicesStorage.begin() + begin);
            std::sort(aIndicesStorage.begin() + begin, aIndicesStorage.begin() + end);
        }
        aIndices = aIndicesStorage.data();
    }

    // Step 3: per-row two-pointer merge of A's neighbors (sorted) and
    // Aᵀ's neighbors (sorted by construction). Equal heads collapse
    // reciprocal pairs (u, v)/(v, u) and self-loops emitted twice into
    // a single entry, mirroring scipy's sparse-addition coalescing.
    // Output stays sorted within each row, so the result is itself a
    // canonical CSR.
    std::vector<int64_t> outIndptr(static_cast<size_t>(n) + 1);
    std::vector<int64_t> outIndices;
    // Upper bound 2m: each input edge contributes at most one (u, v)
    // and one (v, u) pair; coalesced to one when both existed. The
    // actual final size is at most 2m (and typically ≪ m for sparse
    // graphs), so reserve the upper bound and resize down at the end.
    outIndices.reserve(static_cast<size_t>(2 * m));
    outIndptr[0] = 0;
    for (int64_t i = 0; i < n; ++i) {
        auto aIt = aIndices + indptrData[i];
        const auto aEnd = aIndices + indptrData[i + 1];
        auto tIt = atTIndices.begin() + atIndptr[i];
        const auto tEnd = atTIndices.begin() + atIndptr[i + 1];
        int64_t last = -1;
        int64_t count = 0;
        while (aIt != aEnd || tIt != tEnd) {
            int64_t v;
            if (aIt == aEnd) {
                v = *tIt++;
            } else if (tIt == tEnd) {
                v = *aIt++;
            } else if (*aIt < *tIt) {
                v = *aIt++;
            } else if (*tIt < *aIt) {
                v = *tIt++;
            } else {
                // Equal heads: coalesce reciprocal pair or self-loop.
                v = *aIt++;
                ++tIt;
            }
            if (v != last) {
                outIndices.push_back(v);
                last = v;
                ++count;
            }
        }
        outIndptr[static_cast<size_t>(i) + 1] = outIndptr[i] + count;
    }
    outIndices.resize(outIndptr[n]);

    CSRArrowArrays result;
    result.indptr =
        makeCSRArrowArray(std::make_shared<const std::vector<int64_t>>(std::move(outIndptr)));
    result.indices =
        makeCSRArrowArray(std::make_shared<const std::vector<int64_t>>(std::move(outIndices)));
    // edgeIDs intentionally left unset: A + Aᵀ drops per-edge values in
    // scipy's sparse-addition semantics, and a reciprocal pair makes
    // edge identity ambiguous anyway.
    return result;
}

} // namespace main
} // namespace lbug

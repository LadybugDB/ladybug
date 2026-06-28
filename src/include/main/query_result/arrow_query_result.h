#pragma once

#include <memory>
#include <optional>

#include "main/query_result.h"
#include "materialized_query_result.h"

namespace lbug {
namespace main {

class ArrowQueryResult : public QueryResult {
    static constexpr QueryResultType type_ = QueryResultType::ARROW;

public:
    struct CSRMetadata {
        // Dense global indptr. Populated ONLY on the merged result produced
        // by combineCSRChunks()/kwayMergeCSRChunks(); per-batch chunks leave
        // this empty and use the sparse (srcRows, counts) representation
        // below instead, so a batch only pays for the distinct source rows
        // it actually touched (not numSourceRows+1 entries).
        std::vector<int64_t> indptr;
        std::vector<int64_t> indices;
        std::vector<int64_t> edgeIDs;
        // Sparse per-batch CSR representation: srcRows[i] is a global source
        // row id touched by this batch (strictly increasing, since the rel
        // scan emits edges in non-decreasing source order per thread and
        // morsel acquisition is monotonic), and counts[i] is the number of
        // edges for that source row. The edges for srcRows[i] live at
        // indices[offset..offset+counts[i]-1] where offset = sum(counts[0..i-1]).
        // Per-batch srcRows sets are disjoint (a source node is scanned in
        // exactly one morsel -> one thread), so the merge is a union of
        // sorted disjoint runs, not a general k-way merge.
        std::vector<int64_t> srcRows;
        std::vector<int64_t> counts;
        bool hasEdgeIDs = false;
        // Total number of source (node) rows in the table. Used to pad
        // trailing empty rows in indptr; set at plan-mapping time from the
        // node table cardinality.
        int64_t numSourceRows = 0;
    };

    struct CSRArrowArray {
        ArrowArray array{};
        ArrowSchema schema{};

        CSRArrowArray() = default;
        ~CSRArrowArray() { release(); }
        CSRArrowArray(CSRArrowArray&& other) noexcept : array{other.array}, schema{other.schema} {
            other.array.release = nullptr;
            other.schema.release = nullptr;
        }
        CSRArrowArray& operator=(CSRArrowArray&& other) noexcept {
            if (this != &other) {
                release();
                array = other.array;
                schema = other.schema;
                other.array.release = nullptr;
                other.schema.release = nullptr;
            }
            return *this;
        }
        CSRArrowArray(const CSRArrowArray&) = delete;
        CSRArrowArray& operator=(const CSRArrowArray&) = delete;

        void release() {
            if (schema.release) {
                schema.release(&schema);
            }
            if (array.release) {
                array.release(&array);
            }
        }
    };

    struct CSRArrowArrays {
        CSRArrowArray indptr;
        CSRArrowArray indices;
        std::optional<CSRArrowArray> edgeIDs;
    };

    // View of the merged Arrow arrays as a chunked sequence, similar to
    // Arrow C++ lib's arrow::ChunkedArray. We expose per-batch ArrowArrays
    // in batch_index order instead of concatenating them via arrow::Concat
    // (which we don't link); python users can construct pyarrow.ChunkedArray
    // from these chunks and call combine_chunks() to materialize on the
    // consumer side. Chunk order is the natural global row order for the
    // query.
    //
    // Non-owning view: ArrowQueryResult retains ownership of the underlying
    // chunk buffers. The caller is responsible for calling release on each
    // chunk's ArrowArray when done, to free the buffers (matches the
    // existing hasNextArrowChunk / getNextArrowChunk contract).
    struct ArrowChunkedArray {
        std::shared_ptr<const std::vector<ArrowArray>> chunks;

        ArrowChunkedArray() = default;
        explicit ArrowChunkedArray(std::shared_ptr<const std::vector<ArrowArray>> chunks)
            : chunks{std::move(chunks)} {}

        int64_t numChunks() const { return chunks ? static_cast<int64_t>(chunks->size()) : 0; }

        int64_t length() const {
            if (!chunks) {
                return 0;
            }
            int64_t total = 0;
            for (const auto& c : *chunks) {
                total += c.length;
            }
            return total;
        }

        const ArrowArray* data() const {
            return chunks && !chunks->empty() ? chunks->data() : nullptr;
        }

        bool empty() const { return !chunks || chunks->empty(); }
    };

    ArrowQueryResult(std::vector<ArrowArray> arrays, int64_t chunkSize);
    ArrowQueryResult(std::vector<ArrowArray> arrays, int64_t chunkSize,
        std::vector<CSRMetadata> csrChunks);
    ArrowQueryResult(std::vector<std::string> columnNames,
        std::vector<common::LogicalType> columnTypes, processor::FactorizedTable& table,
        int64_t chunkSize);

    uint64_t getNumTuples() const override;

    bool hasNext() const override;

    std::shared_ptr<processor::FlatTuple> getNext() override;

    void resetIterator() override;

    std::string toString() const override;

    bool hasNextArrowChunk() override;

    std::unique_ptr<ArrowArray> getNextArrowChunk(int64_t chunkSize) override;

    // Get all ArrowArrays as a chunked view (ArrowChunkedArray, similar
    // to Arrow C++ lib's arrow::ChunkedArray). Coexists with the
    // hasNextArrowChunk / getNextArrowChunk iteration API; both refer to
    // the same underlying chunks.
    ArrowChunkedArray getArrowChunks() const;

    // CSR metadata is stored as a chunked view: per-batch chunks in
    // batch_index order (the same order the physical collector produces).
    // The actual k-way merge across batches runs lazily on the first call
    // to combineCSRChunks() / getCSRMetadata() / getCSRArrowArrays(); the
    // merged result is cached. NO_ORDER / INSERTION_ORDER queries take
    // zero work at result-construction time.
    bool hasCSRMetadata() const { return !csrChunks.empty(); }
    const CSRMetadata& getCSRMetadata() const { return combineCSRChunks(); }
    CSRArrowArrays getCSRArrowArrays() const;

    // Per-batch CSR chunks (in batch_index order). No merge is performed
    // until combineCSRChunks() is called; this is the ChunkedArray-style
    // view exposed for callers that want to combine on the consumer side
    // (e.g. python users constructing pyarrow.ChunkedArray).
    const std::vector<CSRMetadata>& getCSRChunks() const { return csrChunks; }

    // K-way merge the per-batch CSR chunks in batch_index order into a
    // single CSRMetadata. Result is cached; subsequent calls are O(1).
    const CSRMetadata& combineCSRChunks() const;

private:
    ArrowArray getArray(processor::FactorizedTableIterator& iterator, int64_t chunkSize);

private:
    // Shared with ArrowChunkedArray views so the chunked view and the
    // hasNextArrowChunk / getNextArrowChunk iteration API can coexist
    // without invalidating each other.
    std::shared_ptr<std::vector<ArrowArray>> arraysStorage;
    int64_t chunkSize_;
    uint64_t numTuples = 0;
    uint64_t cursor = 0;
    std::vector<CSRMetadata> csrChunks;
    mutable std::shared_ptr<const CSRMetadata> combinedCSR;
};

} // namespace main
} // namespace lbug

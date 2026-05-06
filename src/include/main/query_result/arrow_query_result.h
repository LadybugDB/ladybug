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
        std::vector<int64_t> indptr;
        std::vector<int64_t> indices;
        std::vector<int64_t> edgeIDs;
        bool hasEdgeIDs = false;
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

    ArrowQueryResult(std::vector<ArrowArray> arrays, int64_t chunkSize);
    ArrowQueryResult(std::vector<ArrowArray> arrays, int64_t chunkSize, CSRMetadata csrMetadata);
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

    bool hasCSRMetadata() const { return csrMetadata != nullptr; }
    const CSRMetadata& getCSRMetadata() const { return *csrMetadata; }
    CSRArrowArrays getCSRArrowArrays() const;

private:
    ArrowArray getArray(processor::FactorizedTableIterator& iterator, int64_t chunkSize);

private:
    std::vector<ArrowArray> arrays;
    int64_t chunkSize_;
    uint64_t numTuples = 0;
    uint64_t cursor = 0;
    std::shared_ptr<const CSRMetadata> csrMetadata;
};

} // namespace main
} // namespace lbug

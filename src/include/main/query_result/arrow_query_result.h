#pragma once

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

    bool hasCSRMetadata() const { return csrMetadata.has_value(); }
    const CSRMetadata& getCSRMetadata() const { return *csrMetadata; }

private:
    ArrowArray getArray(processor::FactorizedTableIterator& iterator, int64_t chunkSize);

private:
    std::vector<ArrowArray> arrays;
    int64_t chunkSize_;
    uint64_t numTuples = 0;
    uint64_t cursor = 0;
    std::optional<CSRMetadata> csrMetadata;
};

} // namespace main
} // namespace lbug

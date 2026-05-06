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

} // namespace

ArrowQueryResult::ArrowQueryResult(std::vector<ArrowArray> arrays, int64_t chunkSize)
    : QueryResult{type_}, arrays{std::move(arrays)}, chunkSize_{chunkSize} {
    for (auto& array : this->arrays) {
        numTuples += array.length;
    }
}

ArrowQueryResult::ArrowQueryResult(std::vector<ArrowArray> arrays, int64_t chunkSize,
    CSRMetadata csrMetadata)
    : QueryResult{type_}, arrays{std::move(arrays)}, chunkSize_{chunkSize},
      csrMetadata{std::make_shared<CSRMetadata>(std::move(csrMetadata))} {
    for (auto& array : this->arrays) {
        numTuples += array.length;
    }
}

ArrowQueryResult::ArrowQueryResult(std::vector<std::string> columnNames,
    std::vector<LogicalType> columnTypes, FactorizedTable& table, int64_t chunkSize)
    : QueryResult{type_, std::move(columnNames), std::move(columnTypes)}, chunkSize_{chunkSize} {
    auto iterator = FactorizedTableIterator(table);
    while (iterator.hasNext()) {
        arrays.push_back(getArray(iterator, chunkSize));
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
    return cursor < arrays.size();
}

std::unique_ptr<ArrowArray> ArrowQueryResult::getNextArrowChunk(int64_t chunkSize) {
    if (chunkSize != chunkSize_) {
        throw RuntimeException(
            std::format("Chunk size does not match expected value {}.", chunkSize_));
    }
    return std::make_unique<ArrowArray>(arrays[cursor++]);
}

ArrowQueryResult::CSRArrowArrays ArrowQueryResult::getCSRArrowArrays() const {
    if (!hasCSRMetadata()) {
        throw RuntimeException("Arrow query result does not have CSR metadata.");
    }
    CSRArrowArrays result;
    result.indptr = makeCSRArrowArray(
        std::shared_ptr<const std::vector<int64_t>>(csrMetadata, &csrMetadata->indptr));
    result.indices = makeCSRArrowArray(
        std::shared_ptr<const std::vector<int64_t>>(csrMetadata, &csrMetadata->indices));
    if (csrMetadata->hasEdgeIDs) {
        result.edgeIDs = makeCSRArrowArray(
            std::shared_ptr<const std::vector<int64_t>>(csrMetadata, &csrMetadata->edgeIDs));
    }
    return result;
}

} // namespace main
} // namespace lbug

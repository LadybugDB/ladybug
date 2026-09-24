#include "processor/result/factorized_table_schema.h"

#include "common/null_buffer.h"

using namespace lbug::common;

namespace lbug {
namespace processor {

ColumnSchema::ColumnSchema(const ColumnSchema& other) {
    isUnFlat = other.isUnFlat;
    groupID = other.groupID;
    numBytes = other.numBytes;
    mayContainNulls = other.mayContainNulls;
}

FactorizedTableSchema::FactorizedTableSchema(const FactorizedTableSchema& other) {
    for (auto i = 0u; i < other.columns.size(); ++i) {
        appendColumn(other.columns[i].copy());
    }
}

void FactorizedTableSchema::appendColumn(ColumnSchema column) {
    // Factorized-table tuples are accessed via direct casts to 8-byte types
    // (hash_t, list_t/string_t overflow pointers, aggregate states holding vptrs).
    // Packing columns without padding leaves those accesses misaligned whenever a
    // preceding small column shifts the offset, which UBSan flags. Align every
    // column offset to 8 bytes and pad the tuple size to 8 so that all tuple bases
    // stay 8-byte aligned (block bases from malloc/page pinning are aligned).
    constexpr uint32_t ALIGNMENT = 8;
    auto alignUp = [](uint32_t v, uint32_t align) { return (v + align - 1) / align * align; };
    if (!colOffsets.empty()) {
        numBytesForDataPerTuple = alignUp(numBytesForDataPerTuple, ALIGNMENT);
    }
    colOffsets.push_back(numBytesForDataPerTuple);
    numBytesForDataPerTuple += column.getNumBytes();
    columns.push_back(std::move(column));
    numBytesForNullMapPerTuple = NullBuffer::getNumBytesForNullValues(getNumColumns());
    numBytesPerTuple = alignUp(numBytesForDataPerTuple + numBytesForNullMapPerTuple, ALIGNMENT);
}

bool FactorizedTableSchema::operator==(const FactorizedTableSchema& other) const {
    if (columns.size() != other.columns.size()) {
        return false;
    }
    for (auto i = 0u; i < columns.size(); i++) {
        if (columns[i] != other.columns[i]) {
            return false;
        }
    }
    return numBytesForDataPerTuple == other.numBytesForDataPerTuple && numBytesForNullMapPerTuple &&
           other.numBytesForNullMapPerTuple;
}

uint64_t FactorizedTableSchema::getNumFlatColumns() const {
    auto numFlatColumns = 0u;
    for (auto& column : columns) {
        if (column.isFlat()) {
            numFlatColumns++;
        }
    }
    return numFlatColumns;
}

uint64_t FactorizedTableSchema::getNumUnFlatColumns() const {
    auto numUnflatColumns = 0u;
    for (auto& column : columns) {
        if (!column.isFlat()) {
            numUnflatColumns++;
        }
    }
    return numUnflatColumns;
}

} // namespace processor
} // namespace lbug

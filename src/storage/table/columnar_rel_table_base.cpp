#include "storage/table/columnar_rel_table_base.h"

#include <algorithm>

#include "common/exception/runtime.h"
#include "main/client_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::transaction;

namespace lbug {
namespace storage {

void ColumnarRelTableScanState::setToTable(const Transaction* transaction, Table* table_,
    std::vector<column_id_t> columnIDs_, std::vector<ColumnPredicateSet> columnPredicateSets_,
    RelDataDirection direction_) {
    // Call base class implementation but skip local table setup: columnar tables do not
    // support local storage.
    TableScanState::setToTable(transaction, table_, std::move(columnIDs_),
        std::move(columnPredicateSets_));
    columns.resize(columnIDs.size());
    direction = direction_;
    for (size_t i = 0; i < columnIDs.size(); ++i) {
        auto columnID = columnIDs[i];
        if (columnID == INVALID_COLUMN_ID || columnID == ROW_IDX_COLUMN_ID) {
            columns[i] = nullptr;
        } else {
            columns[i] = table->cast<RelTable>().getColumn(columnID, direction);
        }
    }
    csrOffsetColumn = table->cast<RelTable>().getCSROffsetColumn(direction);
    csrLengthColumn = table->cast<RelTable>().getCSRLengthColumn(direction);
    nodeGroupIdx = INVALID_NODE_GROUP_IDX;
}

void ColumnarRelTableScanState::reloadCachedBatchData(Transaction* transaction) {
    auto context = transaction->getClientContext();

    // Create DataChunk matching the indices parquet file schema
    auto numIndicesColumns = indicesReader->getNumColumns();
    cachedBatchData = std::make_unique<DataChunk>(numIndicesColumns);

    // Insert value vectors for all columns in the parquet file
    auto memoryManager = MemoryManager::Get(*context);
    for (uint32_t colIdx = 0; colIdx < numIndicesColumns; ++colIdx) {
        const auto& columnTypeRef = indicesReader->getColumnType(colIdx);
        auto columnType = columnTypeRef.copy();
        auto vector = std::make_shared<ValueVector>(std::move(columnType), memoryManager);
        cachedBatchData->insert(colIdx, vector);
    }

    indicesReader->scan(*parquetScanState, *cachedBatchData);
}

common::row_idx_t ColumnarRelTableBase::getNumTotalRows(const Transaction* transaction) {
    return getTotalRowCount(transaction);
}

common::row_idx_t ColumnarRelTableBase::getNumActiveBoundNodes(const Transaction* transaction,
    RelDataDirection direction) {
    return getActiveBoundNodeCount(transaction, direction);
}

std::vector<std::pair<offset_t, row_idx_t>> ColumnarRelTableBase::getDegreeEntries(
    const Transaction* transaction, RelDataDirection direction) {
    return getAllDegreeEntries(transaction, direction);
}

row_idx_t ColumnarRelTableBase::getDegreeForOffset(const Transaction* transaction,
    RelDataDirection direction, offset_t nodeOffset) {
    return getDegreeForOffsetInternal(transaction, direction, nodeOffset);
}

std::vector<std::pair<offset_t, row_idx_t>> ColumnarRelTableBase::getTopKDegrees(
    const Transaction* transaction, RelDataDirection direction, idx_t k) {
    return getTopKDegreeEntries(transaction, direction, k);
}

RelDegreeSummary ColumnarRelTableBase::getDegreeSummary(const Transaction* transaction,
    RelDataDirection direction) {
    auto entries = getAllDegreeEntries(transaction, direction);
    RelDegreeSummary summary;
    summary.numActiveBoundNodes = entries.size();
    for (const auto& [_, degree] : entries) {
        summary.totalDegree += degree;
        summary.maxDegree = std::max(summary.maxDegree, degree);
    }
    return summary;
}

common::offset_t ColumnarRelTableBase::findSourceNodeForRowInternal(offset_t globalRowIdx,
    const std::vector<offset_t>& indptrData) const {
    if (indptrData.empty()) {
        throw RuntimeException("Indptr data not loaded for CSR format");
    }

    // Binary search to find the source node
    // indptrData[i] contains the start row index for source node i
    // Find the largest i where indptrData[i] <= globalRowIdx
    auto it = std::upper_bound(indptrData.begin(), indptrData.end(), globalRowIdx);
    if (it == indptrData.begin()) {
        throw RuntimeException("Invalid global row index: " + std::to_string(globalRowIdx));
    }
    --it;
    return static_cast<offset_t>(std::distance(indptrData.begin(), it));
}

} // namespace storage
} // namespace lbug

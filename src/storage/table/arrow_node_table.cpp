#include "storage/table/arrow_node_table.h"

#include "common/arrow/arrow_converter.h"
#include "common/arrow/arrow_nullmask_tree.h"
#include "common/data_chunk/sel_vector.h"
#include "common/system_config.h"
#include "common/types/types.h"
#include "storage/storage_manager.h"
#include "storage/table/arrow_table_support.h"
#include "transaction/transaction.h"

namespace lbug {
namespace storage {

static uint64_t getArrowBatchLength(const ArrowArrayWrapper& array) {
    if (array.length > 0) {
        return array.length;
    }
    if (array.n_children > 0 && array.children && array.children[0]) {
        return array.children[0]->length;
    }
    return 0;
}

ArrowNodeTable::ArrowNodeTable(const StorageManager* storageManager,
    const catalog::NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager,
    ArrowSchemaWrapper schema, std::vector<ArrowArrayWrapper> arrays, std::string arrowId)
    : ColumnarNodeTableBase{storageManager, nodeTableEntry, memoryManager},
      schema{std::move(schema)}, arrays{std::move(arrays)}, totalRows{0},
      arrowId{std::move(arrowId)} {
    // Note: release may be nullptr if schema is managed by registry
    if (!this->schema.format) {
        throw common::RuntimeException("Arrow schema format cannot be null");
    }
    batchStartOffsets.reserve(this->arrays.size());
    for (const auto& array : this->arrays) {
        batchStartOffsets.push_back(totalRows);
        totalRows += getArrowBatchLength(array);
    }
}

ArrowNodeTable::~ArrowNodeTable() {
    // Unregister Arrow data from the global registry when table is destroyed
    // This handles the case where DROP TABLE is called instead of explicit unregister
    if (!arrowId.empty()) {
        ArrowTableSupport::unregisterArrowData(arrowId);
    }
}

void ArrowNodeTable::initScanState([[maybe_unused]] transaction::Transaction* transaction,
    TableScanState& scanState, [[maybe_unused]] bool resetCachedBoundNodeSelVec) const {
    auto& arrowScanState = scanState.cast<ArrowNodeTableScanState>();

    // Note: We don't copy the schema/arrays as they are wrappers with release callbacks
    arrowScanState.initialized = false;
    arrowScanState.scanCompleted = true;
    arrowScanState.currentBatchIdx = scanState.nodeGroupIdx;
    arrowScanState.currentBatchOffset = 0;
    arrowScanState.nextGlobalRowOffset = 0;
    arrowScanState.totalRows = totalRows;
    arrowScanState.outputToArrowColumnIdx.assign(scanState.columnIDs.size(), -1);
    for (size_t outCol = 0; outCol < scanState.columnIDs.size(); ++outCol) {
        auto columnID = scanState.columnIDs[outCol];
        if (columnID == common::INVALID_COLUMN_ID || columnID == common::ROW_IDX_COLUMN_ID) {
            continue;
        }
        for (common::idx_t propIdx = 0; propIdx < nodeTableCatalogEntry->getNumProperties();
             ++propIdx) {
            if (nodeTableCatalogEntry->getColumnID(propIdx) == columnID) {
                arrowScanState.outputToArrowColumnIdx[outCol] = static_cast<int64_t>(propIdx);
                break;
            }
        }
    }
    if (scanState.source == TableScanSource::COMMITTED &&
        scanState.nodeGroupIdx != common::INVALID_NODE_GROUP_IDX &&
        scanState.nodeGroupIdx < arrays.size()) {
        arrowScanState.scanCompleted = false;
        arrowScanState.nextGlobalRowOffset = batchStartOffsets[scanState.nodeGroupIdx];
    }

    // Each scan state needs to be able to read data independently for parallel scanning
    arrowScanState.initialized = true;
}

static void applySemiMaskFilter(const TableScanState& state, const size_t startOffset,
    const uint64_t numRowsToScan, common::SelectionVector& selVector) {
    const auto endOffset = startOffset + numRowsToScan;
    const auto& arr = state.semiMask->range(startOffset, endOffset);
    if (arr.empty()) {
        selVector.setSelSize(0);
    } else {
        auto stat = selVector.getMutableBuffer();
        uint64_t numSelectedValues = 0;
        size_t i = 0, j = 0;
        while (i < numRowsToScan && j < arr.size()) {
            auto temp = arr[j] - startOffset;
            if (selVector[i] < temp) {
                ++i;
            } else if (selVector[i] > temp) {
                ++j;
            } else {
                stat[numSelectedValues++] = temp;
                ++i;
                ++j;
            }
        }
        selVector.setToFiltered(numSelectedValues);
    }
}

bool ArrowNodeTable::scanInternal([[maybe_unused]] transaction::Transaction* transaction,
    TableScanState& scanState) {
    auto& arrowScanState = scanState.cast<ArrowNodeTableScanState>();
    if (arrowScanState.scanCompleted || arrowScanState.currentBatchIdx >= arrays.size()) {
        return false;
    }

    scanState.resetOutVectors();
    const auto& batch = arrays[arrowScanState.currentBatchIdx];
    auto batchLength = getArrowBatchLength(batch);
    if (arrowScanState.currentBatchOffset >= batchLength) {
        arrowScanState.scanCompleted = true;
        return false;
    }

    auto outputSize = static_cast<uint64_t>(batchLength - arrowScanState.currentBatchOffset);

    scanState.outState->getSelVectorUnsafe().setSelSize(outputSize);

    if (scanState.semiMask && scanState.semiMask->isEnabled()) {
        applySemiMaskFilter(scanState, arrowScanState.nextGlobalRowOffset, outputSize,
            scanState.outState->getSelVectorUnsafe());
        if (scanState.outState->getSelVector().getSelSize() == 0) {
            arrowScanState.currentBatchOffset += outputSize;
            arrowScanState.nextGlobalRowOffset += outputSize;
            return true;
        }
    }

    KU_ASSERT(scanState.outputVectors.size() == arrowScanState.outputToArrowColumnIdx.size());
    copyArrowBatchToOutputVectors(batch, arrowScanState.currentBatchOffset, outputSize,
        scanState.outputVectors, arrowScanState.outputToArrowColumnIdx);

    auto tableID = this->getTableID();
    for (uint64_t i = 0; i < outputSize; ++i) {
        auto& nodeID = scanState.nodeIDVector->getValue<common::nodeID_t>(i);
        nodeID.tableID = tableID;
        nodeID.offset = arrowScanState.nextGlobalRowOffset + i;
    }

    arrowScanState.currentBatchOffset += outputSize;
    arrowScanState.nextGlobalRowOffset += outputSize;
    return true;
}

common::node_group_idx_t ArrowNodeTable::getNumBatches(
    [[maybe_unused]] const transaction::Transaction* transaction) const {
    return arrays.size();
}

common::row_idx_t ArrowNodeTable::getTotalRowCount(
    [[maybe_unused]] const transaction::Transaction* transaction) const {
    return totalRows;
}

void ArrowNodeTable::copyArrowBatchToOutputVectors(const ArrowArrayWrapper& batch,
    const size_t currentBatchOffset, const uint64_t numRowsToCopy,
    const std::vector<common::ValueVector*>& outputVectors,
    const std::vector<int64_t>& outputToArrowColumnIdx) const {
    auto numChildren = batch.n_children < 0 ? 0u : static_cast<uint64_t>(batch.n_children);

    for (uint64_t outCol = 0; outCol < outputVectors.size(); ++outCol) {
        if (!outputVectors[outCol]) {
            continue;
        }
        auto arrowColIdx = outputToArrowColumnIdx[outCol];
        if (arrowColIdx < 0 || static_cast<uint64_t>(arrowColIdx) >= numChildren ||
            !batch.children || !schema.children || !batch.children[arrowColIdx] ||
            !schema.children[arrowColIdx]) {
            continue;
        }
        auto& outputVector = *outputVectors[outCol];
        auto* childArray = batch.children[arrowColIdx];
        auto* childSchema = schema.children[arrowColIdx];
        common::ArrowNullMaskTree nullMask(childSchema, childArray, childArray->offset,
            childArray->length);
        common::ArrowConverter::fromArrowArray(childSchema, childArray, outputVector, &nullMask,
            childArray->offset + currentBatchOffset, 0, numRowsToCopy);
    }
}

} // namespace storage
} // namespace lbug

#include "storage/table/arrow_node_table.h"

#include <algorithm>

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
    : ColumnarNodeTableBase{storageManager, nodeTableEntry, memoryManager,
          std::make_unique<ArrowNodeTableScanSharedState>(scanMorselSize)},
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

void ArrowNodeTable::initializeScanCoordination(const transaction::Transaction* transaction) {
    auto arrowScanSharedState =
        static_cast<ArrowNodeTableScanSharedState*>(tableScanSharedState.get());
    auto batchSizes = getBatchSizes(transaction);
    arrowScanSharedState->reset(batchSizes);
}

void ArrowNodeTable::initScanState([[maybe_unused]] transaction::Transaction* transaction,
    TableScanState& scanState, [[maybe_unused]] bool resetCachedBoundNodeSelVec) const {
    auto& arrowScanState = scanState.cast<ArrowNodeTableScanState>();

    // Note: We don't copy the schema/arrays as they are wrappers with release callbacks
    arrowScanState.initialized = false;
    arrowScanState.scanCompleted = true;
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

    if (arrowScanState.source == TableScanSource::COMMITTED &&
        arrowScanState.currentBatchIdx != static_cast<size_t>(common::INVALID_NODE_GROUP_IDX) &&
        arrowScanState.currentBatchIdx < arrays.size()) {
        arrowScanState.scanCompleted = false;
    }

    // Each scan state needs to be able to read data independently for parallel scanning
    arrowScanState.initialized = true;
}

// First run always fails due to arrowScanState.scanCompleted == true because either
// scanState.source = NONE or scanState.currentBatchIdx = INVALID_NODE_GROUP_IDX on the first
// run(look at initScanState function) tableScanSharedState.nextMorsel will drive scanInternal
// completely
bool ArrowNodeTable::scanInternal([[maybe_unused]] transaction::Transaction* transaction,
    TableScanState& scanState) {
    auto& arrowScanState = scanState.cast<ArrowNodeTableScanState>();
    if (arrowScanState.scanCompleted) {
        return false;
    }

    if (arrowScanState.currentBatchIdx >= arrays.size() ||
        arrowScanState.currentMorselStartOffset >= arrowScanState.currentMorselEndOffset) {
        arrowScanState.scanCompleted = true;
        return false;
    }

    const auto& batch = arrays[arrowScanState.currentBatchIdx];
    auto batchLength = getArrowBatchLength(batch);

    if (batchLength == 0 || !batch.children || !schema.children || batch.n_children <= 0) {
        arrowScanState.scanCompleted = true;
        return false;
    }

    scanState.resetOutVectors();

    // Calculate the size of the current morsel
    auto morselStart = arrowScanState.currentMorselStartOffset;
    auto morselEnd = std::min((uint64_t)arrowScanState.currentMorselEndOffset, batchLength);
    auto outputSize = static_cast<uint64_t>(morselEnd - morselStart);

    auto nextGlobalRowOffset = batchStartOffsets[arrowScanState.currentBatchIdx] + morselStart;

    scanState.outState->getSelVectorUnsafe().setSelSize(outputSize);

    NodeTable::applySemiMaskFilter(scanState, nextGlobalRowOffset, outputSize,
        scanState.outState->getSelVectorUnsafe());

    if (scanState.outState->getSelVector().getSelSize() == 0) {
        return false;
    }

    DASSERT(scanState.outputVectors.size() == arrowScanState.outputToArrowColumnIdx.size());
    copyArrowMorselToOutputVectors(batch, arrowScanState.currentMorselStartOffset, outputSize,
        scanState.outputVectors, arrowScanState.outputToArrowColumnIdx);

    auto tableID = this->getTableID();
    for (uint64_t i = 0; i < outputSize; ++i) {
        auto& nodeID = scanState.nodeIDVector->getValue<common::nodeID_t>(i);
        nodeID.tableID = tableID;
        nodeID.offset = nextGlobalRowOffset + i;
    }

    arrowScanState.currentMorselStartOffset += outputSize;

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

std::vector<size_t> ArrowNodeTable::getBatchSizes(
    [[maybe_unused]] const transaction::Transaction* transaction) const {
    std::vector<size_t> batchSizes;

    for (const auto& array : arrays) {
        batchSizes.push_back(getArrowBatchLength(array));
    }

    return batchSizes;
}

size_t ArrowNodeTable::getNumScanMorsels(
    [[maybe_unused]] const transaction::Transaction* transaction) const {
    size_t numMorsels = 0;
    for (const auto& array : arrays) {
        auto batchLength = getArrowBatchLength(array);
        numMorsels += (batchLength + scanMorselSize - 1) / scanMorselSize;
    }
    return numMorsels;
}

void ArrowNodeTable::copyArrowMorselToOutputVectors(const ArrowArrayWrapper& batch,
    const size_t currentMorselStartOffset, const uint64_t numRowsToCopy,
    const std::vector<common::ValueVector*>& outputVectors,
    const std::vector<int64_t>& outputToArrowColumnIdx) const {
    auto numChildren = static_cast<uint64_t>(batch.n_children);

    for (uint64_t outCol = 0; outCol < outputVectors.size(); ++outCol) {
        if (!outputVectors[outCol]) {
            continue;
        }
        auto arrowColIdx = outputToArrowColumnIdx[outCol];
        if (arrowColIdx < 0 || static_cast<uint64_t>(arrowColIdx) >= numChildren ||
            !batch.children[arrowColIdx] || !schema.children[arrowColIdx]) {
            continue;
        }
        auto& outputVector = *outputVectors[outCol];
        auto* childArray = batch.children[arrowColIdx];
        auto* childSchema = schema.children[arrowColIdx];
        common::ArrowNullMaskTree nullMask(childSchema, childArray, childArray->offset,
            childArray->length);
        common::ArrowConverter::fromArrowArray(childSchema, childArray, outputVector, &nullMask,
            childArray->offset + currentMorselStartOffset, 0, numRowsToCopy);
    }
}

} // namespace storage
} // namespace lbug

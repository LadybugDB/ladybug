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
    arrowScanState.currentBatchOffset = 0;
    arrowScanState.nextGlobalRowOffset = 0;
    arrowScanState.currentMorselStartOffset = 0;
    arrowScanState.currentMorselEndOffset = 0;
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

    // Set nodeGroupIdx to invalid initially - will be assigned by getNextBatch via
    // initArrowScanForBatch
    arrowScanState.nodeGroupIdx = common::INVALID_NODE_GROUP_IDX;

    // Initialize scan state for the current batch (assigned via shared state)
    // This fetches the first batch from the shared state
    initArrowScanForBatch(transaction, arrowScanState);

    // Each scan state needs to be able to read data independently for parallel scanning
    arrowScanState.initialized = true;
}

void ArrowNodeTable::initArrowScanForBatch([[maybe_unused]] transaction::Transaction* transaction,
    ArrowNodeTableScanState& scanState) const {
    // Use shared state to get the next available batch for this scan state
    if (scanState.nodeGroupIdx == common::INVALID_NODE_GROUP_IDX) {
        common::node_group_idx_t assignedBatchIdx;
        if (sharedState->getNextBatch(assignedBatchIdx)) {
            scanState.nodeGroupIdx = assignedBatchIdx;
            scanState.currentBatchIdx = assignedBatchIdx;
            scanState.currentBatchOffset = 0;
            scanState.nextGlobalRowOffset = batchStartOffsets[assignedBatchIdx];
            scanState.scanCompleted = false;

            // Initialize morsel boundaries for the first morsel in this batch
            auto batchLength = getArrowBatchLength(arrays[assignedBatchIdx]);
            scanState.currentMorselStartOffset = 0;
            scanState.currentMorselEndOffset = std::min(scanState.morselSize, batchLength);
        } else {
            // No more batches available - mark scan as completed
            scanState.scanCompleted = true;
            return;
        }
    } else {
        // Batch already assigned (e.g., by external morsel system or re-initialization)
        scanState.currentBatchIdx = scanState.nodeGroupIdx;
        scanState.currentBatchOffset = 0;
        scanState.nextGlobalRowOffset = batchStartOffsets[scanState.nodeGroupIdx];
        scanState.scanCompleted = false;

        // Initialize morsel boundaries for the first morsel
        auto batchLength = getArrowBatchLength(arrays[scanState.nodeGroupIdx]);
        scanState.currentMorselStartOffset = 0;
        scanState.currentMorselEndOffset = std::min(scanState.morselSize, batchLength);
    }
}

bool ArrowNodeTable::scanInternal([[maybe_unused]] transaction::Transaction* transaction,
    TableScanState& scanState) {
    auto& arrowScanState = scanState.cast<ArrowNodeTableScanState>();
    if (arrowScanState.scanCompleted) {
        return false;
    }

    // Check if we need to move to the next morsel or batch
    if (arrowScanState.currentBatchIdx >= arrays.size()) {
        arrowScanState.scanCompleted = true;
        return false;
    }

    const auto& batch = arrays[arrowScanState.currentBatchIdx];
    auto batchLength = getArrowBatchLength(batch);

    // Check if current morsel is exhausted, advance to next morsel
    if (arrowScanState.currentMorselStartOffset >= batchLength) {
        // All morsels in current batch exhausted, try to get next batch
        arrowScanState.nodeGroupIdx = common::INVALID_NODE_GROUP_IDX;
        initArrowScanForBatch(transaction, arrowScanState);
        if (arrowScanState.scanCompleted) {
            return false; // No more batches available
        }
        // Refresh batch reference after getting new batch
        const auto& newBatch = arrays[arrowScanState.currentBatchIdx];
        batchLength = getArrowBatchLength(newBatch);
    }

    scanState.resetOutVectors();

    // Calculate the size of the current morsel
    auto morselStart = arrowScanState.currentMorselStartOffset;
    auto morselEnd = std::min(arrowScanState.currentMorselEndOffset, batchLength);
    auto outputSize = static_cast<uint64_t>(morselEnd - morselStart);

    // Update batch offset for this morsel
    arrowScanState.currentBatchOffset = morselStart;
    arrowScanState.nextGlobalRowOffset =
        batchStartOffsets[arrowScanState.currentBatchIdx] + morselStart;

    scanState.outState->getSelVectorUnsafe().setSelSize(outputSize);

    if (scanState.semiMask && scanState.semiMask->isEnabled()) {
        applySemiMaskFilter(scanState, arrowScanState.nextGlobalRowOffset, outputSize,
            scanState.outState->getSelVectorUnsafe());
        if (scanState.outState->getSelVector().getSelSize() == 0) {
            // Advance to next morsel
            arrowScanState.currentMorselStartOffset = morselEnd;
            arrowScanState.currentMorselEndOffset = morselEnd + arrowScanState.morselSize;
            return true;
        }
    }

    DASSERT(scanState.outputVectors.size() == arrowScanState.outputToArrowColumnIdx.size());
    copyArrowBatchToOutputVectors(batch, arrowScanState.currentBatchOffset, outputSize,
        scanState.outputVectors, arrowScanState.outputToArrowColumnIdx);

    auto tableID = this->getTableID();
    for (uint64_t i = 0; i < outputSize; ++i) {
        auto& nodeID = scanState.nodeIDVector->getValue<common::nodeID_t>(i);
        nodeID.tableID = tableID;
        nodeID.offset = arrowScanState.nextGlobalRowOffset + i;
    }

    // Advance to next morsel
    arrowScanState.currentMorselStartOffset = morselEnd;
    arrowScanState.currentMorselEndOffset = morselEnd + arrowScanState.morselSize;
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

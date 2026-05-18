#include "storage/table/ice_mem_node_table.h"

#include <algorithm>

#include "common/arrow/arrow_converter.h"
#include "common/data_chunk/sel_vector.h"
#include "common/system_config.h"
#include "common/types/types.h"
#include "storage/storage_manager.h"
#include "storage/table/arrow_table_support.h"
#include "storage/table/arrow_utils.h"
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

IceMemNodeTable::IceMemNodeTable(const StorageManager* storageManager,
    const catalog::NodeTableCatalogEntry* entry, MemoryManager* memoryManager)
    : ColumnarNodeTableBase{storageManager, entry, memoryManager,
          std::make_unique<IceMemNodeTableScanSharedState>(scanMorselSize)},
      totalRows{0} {

    // Extract Arrow ID from storage string
    arrowId = entry->getStorage();

    // Retrieve Arrow data from registry (as pointers to registry data)
    ArrowSchemaWrapper* schemaCopy = nullptr;
    std::vector<ArrowArrayWrapper>* arraysCopy = nullptr;
    if (!ArrowTableSupport::getArrowData(arrowId, schemaCopy, arraysCopy)) {
        throw common::RuntimeException("Failed to retrieve arrow data table with ID: " + arrowId);
    }

    // Create wrappers that reference registry memory while registry keeps ownership.
    schema = createShallowCopy(*schemaCopy);

    arrays.reserve(arraysCopy->size());
    for (const auto& arr : *arraysCopy) {
        arrays.push_back(createShallowCopy(arr));
    }

    if (!this->schema.format) {
        throw common::RuntimeException("IceMemNodeTable Arrow schema format cannot be null");
    }

    batchStartOffsets.reserve(this->arrays.size());

    for (const auto& array : this->arrays) {
        batchStartOffsets.push_back(totalRows);
        totalRows += getArrowBatchLength(array);
    }
}

IceMemNodeTable::~IceMemNodeTable() {
    // Unregister Arrow data from the global registry when table is destroyed
    // This handles the case where DROP TABLE is called instead of explicit unregister
    if (!arrowId.empty()) {
        ArrowTableSupport::unregisterArrowData(arrowId);
    }
}

void IceMemNodeTable::initializeScanCoordination(const transaction::Transaction* transaction) {
    auto iceMemScanSharedState =
        static_cast<IceMemNodeTableScanSharedState*>(tableScanSharedState.get());
    auto batchSizes = getBatchSizes(transaction);
    iceMemScanSharedState->reset(batchSizes);
}

void IceMemNodeTable::initScanState([[maybe_unused]] transaction::Transaction* transaction,
    TableScanState& scanState, [[maybe_unused]] bool resetCachedBoundNodeSelVec) const {
    auto& iceMemScanState = scanState.cast<IceMemNodeTableScanState>();

    // Note: We don't copy the schema/arrays as they are wrappers with release callbacks
    iceMemScanState.initialized = false;
    iceMemScanState.scanCompleted = true;

    if (iceMemScanState.source == TableScanSource::COMMITTED &&
        iceMemScanState.currentBatchIdx != static_cast<size_t>(common::INVALID_NODE_GROUP_IDX) &&
        iceMemScanState.currentBatchIdx < arrays.size()) {
        iceMemScanState.scanCompleted = false;
    }

    // Each scan state needs to be able to read data independently for parallel scanning
    iceMemScanState.initialized = true;
}

// First run always fails due to iceMemScanState.scanCompleted == true because either
// scanState.source = NONE or scanState.currentBatchIdx = INVALID_NODE_GROUP_IDX on the first
// run(look at initScanState function) tableScanSharedState.nextMorsel will drive scanInternal
// completely
bool IceMemNodeTable::scanInternal([[maybe_unused]] transaction::Transaction* transaction,
    TableScanState& scanState) {
    auto& iceMemScanState = scanState.cast<IceMemNodeTableScanState>();
    if (iceMemScanState.scanCompleted) {
        return false;
    }

    if (iceMemScanState.currentBatchIdx >= arrays.size() ||
        iceMemScanState.currentMorselStartOffset >= iceMemScanState.currentMorselEndOffset) {
        iceMemScanState.scanCompleted = true;
        return false;
    }

    const auto& batch = arrays[iceMemScanState.currentBatchIdx];
    auto batchLength = getArrowBatchLength(batch);

    if (batchLength == 0 || !batch.children || !schema.children || batch.n_children <= 0) {
        iceMemScanState.scanCompleted = true;
        return false;
    }

    scanState.resetOutVectors();

    // Calculate the size of the current morsel
    auto morselStart = iceMemScanState.currentMorselStartOffset;
    auto morselEnd = std::min((uint64_t)iceMemScanState.currentMorselEndOffset, batchLength);
    auto outputSize = static_cast<uint64_t>(morselEnd - morselStart);

    auto nextGlobalRowOffset = batchStartOffsets[iceMemScanState.currentBatchIdx] + morselStart;

    scanState.outState->getSelVectorUnsafe().setSelSize(outputSize);

    NodeTable::applySemiMaskFilter(scanState, nextGlobalRowOffset, outputSize,
        scanState.outState->getSelVectorUnsafe());

    if (scanState.outState->getSelVector().getSelSize() == 0) {
        return false;
    }

    const auto outputToArrowColumnIdx = getOutputToArrowColumnIdx(scanState.columnIDs);
    DASSERT(scanState.outputVectors.size() == outputToArrowColumnIdx.size());
    ArrowUtils::copyArrowMorselToOutputVectors(batch, schema,
        iceMemScanState.currentMorselStartOffset, outputSize, scanState.outputVectors,
        outputToArrowColumnIdx);

    auto tableID = this->getTableID();
    for (uint64_t i = 0; i < outputSize; ++i) {
        auto& nodeID = scanState.nodeIDVector->getValue<common::nodeID_t>(i);
        nodeID.tableID = tableID;
        nodeID.offset = nextGlobalRowOffset + i;
    }

    iceMemScanState.currentMorselStartOffset += outputSize;

    return true;
}

common::node_group_idx_t IceMemNodeTable::getNumBatches(
    [[maybe_unused]] const transaction::Transaction* transaction) const {
    return arrays.size();
}

common::row_idx_t IceMemNodeTable::getTotalRowCount(
    [[maybe_unused]] const transaction::Transaction* transaction) const {
    return totalRows;
}

std::vector<size_t> IceMemNodeTable::getBatchSizes(
    [[maybe_unused]] const transaction::Transaction* transaction) const {
    std::vector<size_t> batchSizes;

    for (const auto& array : arrays) {
        batchSizes.push_back(getArrowBatchLength(array));
    }

    return batchSizes;
}

size_t IceMemNodeTable::getNumScanMorsels(
    [[maybe_unused]] const transaction::Transaction* transaction) const {
    size_t numMorsels = 0;
    for (const auto& array : arrays) {
        auto batchLength = getArrowBatchLength(array);
        numMorsels += (batchLength + scanMorselSize - 1) / scanMorselSize;
    }
    return numMorsels;
}

std::vector<int64_t> IceMemNodeTable::getOutputToArrowColumnIdx(
    const std::vector<common::column_id_t>& columnIDs) const {
    std::vector<int64_t> outputToArrowColumnIdx(columnIDs.size(), -1);
    for (size_t col = 0; col < columnIDs.size(); ++col) {
        const auto columnID = columnIDs[col];
        if (columnID == common::INVALID_COLUMN_ID || columnID == common::ROW_IDX_COLUMN_ID) {
            continue;
        }
        for (common::idx_t propIdx = 0; propIdx < nodeTableCatalogEntry->getNumProperties();
             ++propIdx) {
            if (nodeTableCatalogEntry->getColumnID(propIdx) == columnID) {
                outputToArrowColumnIdx[col] = static_cast<int64_t>(propIdx);
                break;
            }
        }
    }
    return outputToArrowColumnIdx;
}

bool IceMemNodeTable::isVisible([[maybe_unused]] const transaction::Transaction* transaction,
    common::offset_t offset) const {
    return offset < totalRows;
}

bool IceMemNodeTable::isVisibleNoLock([[maybe_unused]] const transaction::Transaction* transaction,
    common::offset_t offset) const {
    return offset < totalRows;
}

} // namespace storage
} // namespace lbug

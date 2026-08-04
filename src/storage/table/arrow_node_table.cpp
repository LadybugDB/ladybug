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

static std::vector<std::optional<common::ArrowLogicalTypeInfo>> resolveColumnLogicalTypeInfos(
    const ArrowSchemaWrapper& schema) {
    std::vector<std::optional<common::ArrowLogicalTypeInfo>> result;
    if (schema.n_children <= 0 || schema.children == nullptr) {
        return result;
    }
    result.reserve(static_cast<size_t>(schema.n_children));
    for (int64_t i = 0; i < schema.n_children; ++i) {
        result.push_back(schema.children[i] == nullptr ?
                             std::nullopt :
                             common::tryGetArrowLogicalTypeInfo(schema.children[i]));
    }
    return result;
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
    columnLogicalTypeInfos = resolveColumnLogicalTypeInfos(this->schema);
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

    const auto outputToArrowColumnIdx = getOutputToArrowColumnIdx(scanState.columnIDs);
    DASSERT(scanState.outputVectors.size() == outputToArrowColumnIdx.size());
    copyArrowMorselToOutputVectors(batch, arrowScanState.currentMorselStartOffset, outputSize,
        scanState.outputVectors, outputToArrowColumnIdx);

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

std::vector<int64_t> ArrowNodeTable::getOutputToArrowColumnIdx(
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
        const auto* logicalTypeInfo =
            static_cast<size_t>(arrowColIdx) < columnLogicalTypeInfos.size() ?
                &columnLogicalTypeInfos[arrowColIdx] :
                nullptr;
        common::ArrowNullMaskTree nullMask(childSchema, childArray, childArray->offset,
            childArray->length);
        common::ArrowConverter::fromArrowArray(childSchema, childArray, outputVector, &nullMask,
            childArray->offset + currentMorselStartOffset, 0, numRowsToCopy, logicalTypeInfo);
    }
}

bool ArrowNodeTable::lookupPK([[maybe_unused]] const transaction::Transaction* transaction,
    common::ValueVector* keyVector, uint64_t vectorPos, common::offset_t& result) const {
    if (keyVector->isNull(vectorPos)) {
        return false;
    }

    int64_t pkArrowColumnIdx = -1;
    for (common::idx_t propIdx = 0; propIdx < nodeTableCatalogEntry->getNumProperties();
         ++propIdx) {
        if (nodeTableCatalogEntry->getColumnID(propIdx) == getPKColumnID()) {
            pkArrowColumnIdx = static_cast<int64_t>(propIdx);
            break;
        }
    }
    if (pkArrowColumnIdx < 0 || !schema.children || pkArrowColumnIdx >= schema.n_children ||
        !schema.children[pkArrowColumnIdx]) {
        return false;
    }

    auto keyToLookup = keyVector->getAsValue(vectorPos);
    auto pkType = getColumn(getPKColumnID()).getDataType().copy();
    auto singleValueState = common::DataChunkState::getSingleValueDataChunkState();
    auto arrowPKVector =
        std::make_unique<common::ValueVector>(std::move(pkType), memoryManager, singleValueState);
    arrowPKVector->state->setToFlat();

    for (size_t batchIdx = 0; batchIdx < arrays.size(); ++batchIdx) {
        const auto& batch = arrays[batchIdx];
        const auto batchLength = getArrowBatchLength(batch);
        if (batchLength == 0 || !batch.children || pkArrowColumnIdx >= batch.n_children ||
            !batch.children[pkArrowColumnIdx]) {
            continue;
        }
        auto* pkChildArray = batch.children[pkArrowColumnIdx];
        auto* pkChildSchema = schema.children[pkArrowColumnIdx];
        const auto* logicalTypeInfo =
            static_cast<size_t>(pkArrowColumnIdx) < columnLogicalTypeInfos.size() ?
                &columnLogicalTypeInfos[pkArrowColumnIdx] :
                nullptr;
        common::ArrowNullMaskTree nullMask(pkChildSchema, pkChildArray, pkChildArray->offset,
            pkChildArray->length);
        for (uint64_t rowIdx = 0; rowIdx < batchLength; ++rowIdx) {
            common::ArrowConverter::fromArrowArray(pkChildSchema, pkChildArray, *arrowPKVector,
                &nullMask, pkChildArray->offset + rowIdx, 0, 1, logicalTypeInfo);
            if (arrowPKVector->isNull(0)) {
                continue;
            }
            if (*arrowPKVector->getAsValue(0) == *keyToLookup) {
                result = batchStartOffsets[batchIdx] + rowIdx;
                return true;
            }
        }
    }
    return false;
}

bool ArrowNodeTable::isVisible([[maybe_unused]] const transaction::Transaction* transaction,
    common::offset_t offset) const {
    return offset < totalRows;
}

bool ArrowNodeTable::isVisibleNoLock([[maybe_unused]] const transaction::Transaction* transaction,
    common::offset_t offset) const {
    return offset < totalRows;
}

} // namespace storage
} // namespace lbug

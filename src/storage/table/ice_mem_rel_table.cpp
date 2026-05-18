#include "storage/table/ice_mem_rel_table.h"

#include <cstring>

#include "common/arrow/arrow_converter.h"
#include "common/data_chunk/sel_vector.h"
#include "common/exception/runtime.h"
#include "common/system_config.h"
#include "common/types/internal_id_util.h"
#include "storage/table/arrow_table_support.h"
#include "storage/table/arrow_utils.h"
#include "storage/table/csr_node_group.h"
#include "transaction/transaction.h"

namespace lbug {
namespace storage {

using namespace common;

void IceMemRelTableScanState::setToTable(const transaction::Transaction* transaction, Table* table_,
    std::vector<column_id_t> columnIDs_, std::vector<ColumnPredicateSet> columnPredicateSets_,
    RelDataDirection direction_) {
    // Same behavior as IceDiskRelTable: no local table for external data sources.
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

IceMemRelTable::IceMemRelTable(catalog::RelGroupCatalogEntry* entry, table_id_t fromTableID,
    table_id_t toTableID, const StorageManager* storageManager, MemoryManager* memoryManager)
    : ColumnarRelTableBase{entry, fromTableID, toTableID, storageManager, memoryManager} {

    // store indices and indptr arrow arrays
    std::string indicesArrowId = "";
    std::string indptrArrowId = "";

    ArrowSchemaWrapper* schema = nullptr;
    std::vector<ArrowArrayWrapper>* arrays = nullptr;

    // indices
    if (!ArrowTableSupport::getArrowData(indicesArrowId, schema, arrays)) {
        throw common::RuntimeException(
            "Failed to retrieve arrow data for icebug-memory indices table with ID: " +
            indicesArrowId);
    }

    if (!schema->format || schema->n_children <= 0 || !schema->children || !schema->children[0]) {
        throw RuntimeException(
            "Invalid arrow schema for icebug-memory indices table with ID: " + indicesArrowId);
    }

    schema = nullptr;
    arrays = nullptr;
    indicesSchema = createShallowCopy(*schema);
    indices.reserve(arrays->size());
    for (const auto& arr : *arrays) {
        indices.push_back(createShallowCopy(arr));
    }

    // indptr
    if (!ArrowTableSupport::getArrowData(indptrArrowId, schema, arrays)) {
        throw common::RuntimeException(
            "Failed to retrieve arrow data for icebug-memory indptr table with ID: " +
            indptrArrowId);
    }

    if (!schema->format || schema->n_children <= 0 || !schema->children || !schema->children[0]) {
        throw RuntimeException(
            "Invalid arrow schema for icebug-memory indptr table with ID: " + indptrArrowId);
    }

    indptrSchema = createShallowCopy(*schema);
    indptr.reserve(arrays->size());
    for (const auto& arr : *arrays) {
        indptr.push_back(createShallowCopy(arr));
    }

    for (const auto& prop : entry->getProperties()) {
        if (prop.getName() == "_ID") {
            continue;
        }

        auto columnID = entry->getColumnID(prop.getName());
        if (columnID == NBR_ID_COLUMN_ID || columnID == REL_ID_COLUMN_ID) {
            continue;
        }

        auto arrowColIdx = ArrowUtils::findColumnIdx(indicesSchema, prop.getName());
        if (arrowColIdx < 0) {
            throw RuntimeException("Missing property column '" + prop.getName() +
                                   "' in icebug-memory indices table with ID: " + indicesArrowId);
        }

        propertyColumnToArrowColumnIdx[columnID] = arrowColIdx;
    }

    for (const auto& array : indices) {
        batchStartOffsets.push_back(totalIndicesRows);
        totalIndicesRows += ArrowUtils::getArrowBatchLength(array);
    }
}

IceMemRelTable::~IceMemRelTable() {
    std::string indicesArrowId = "";
    std::string indptrArrowId = "";

    if (!indicesArrowId.empty()) {
        ArrowTableSupport::unregisterArrowData(indicesArrowId);
    }

    if (!indptrArrowId.empty()) {
        ArrowTableSupport::unregisterArrowData(indptrArrowId);
    }
}

void IceMemRelTable::initScanState([[maybe_unused]] transaction::Transaction* transaction,
    TableScanState& scanState, bool resetCachedBoundNodeSelVec) const {
    auto& relScanState = scanState.cast<RelTableScanState>();
    relScanState.source = TableScanSource::COMMITTED;
    relScanState.nodeGroup = nullptr;
    relScanState.nodeGroupIdx = INVALID_NODE_GROUP_IDX;

    if (resetCachedBoundNodeSelVec) {
        if (relScanState.nodeIDVector->state->getSelVector().isUnfiltered()) {
            relScanState.cachedBoundNodeSelVector.setToUnfiltered();
        } else {
            relScanState.cachedBoundNodeSelVector.setToFiltered();
            memcpy(relScanState.cachedBoundNodeSelVector.getMutableBuffer().data(),
                relScanState.nodeIDVector->state->getSelVector().getMutableBuffer().data(),
                relScanState.nodeIDVector->state->getSelVector().getSelSize() * sizeof(sel_t));
        }
        relScanState.cachedBoundNodeSelVector.setSelSize(
            relScanState.nodeIDVector->state->getSelVector().getSelSize());
    }

    relScanState.arrowBoundNodeOffsetToSelPos.clear();
    for (uint64_t i = 0; i < relScanState.cachedBoundNodeSelVector.getSelSize(); ++i) {
        auto boundNodeIdx = relScanState.cachedBoundNodeSelVector[i];
        const auto boundNodeID = relScanState.nodeIDVector->getValue<nodeID_t>(boundNodeIdx);
        relScanState.arrowBoundNodeOffsetToSelPos.emplace(boundNodeID.offset, boundNodeIdx);
    }

    relScanState.arrowCurrentBatchIdx = 0;
    relScanState.arrowCurrentBatchOffset = 0;
    relScanState.arrowScanCompleted = indices.empty();
}

bool IceMemRelTable::scanInternal(transaction::Transaction* /*transaction*/,
    TableScanState& scanState) {
    auto& relScanState = scanState.cast<RelTableScanState>();
    if (relScanState.arrowScanCompleted || relScanState.arrowBoundNodeOffsetToSelPos.empty()) {
        relScanState.outState->getSelVectorUnsafe().setToFiltered(0);
        return false;
    }

    scanState.resetOutVectors();

    const auto isFwd = relScanState.direction != RelDataDirection::BWD;
    auto outputCount = 0u;
    constexpr uint64_t maxRowsPerCall = DEFAULT_VECTOR_CAPACITY;
    auto activeBoundSelPos = INVALID_SEL;
    auto activeBoundOffset = INVALID_OFFSET;
    auto hasActiveBound = false;

    while (outputCount < maxRowsPerCall && relScanState.arrowCurrentBatchIdx < indices.size()) {
        const auto& batch = indices[relScanState.arrowCurrentBatchIdx];
        auto batchLength = ArrowUtils::getArrowBatchLength(batch);

        // batch related checks
        if (relScanState.arrowCurrentBatchOffset >= batchLength || batch.n_children <= 0 ||
            !batch.children || !batch.children[0]) {
            relScanState.arrowCurrentBatchIdx++;
            relScanState.arrowCurrentBatchOffset = 0;
            continue;
        }

        auto relOffset = batchStartOffsets[relScanState.arrowCurrentBatchIdx] +
                         relScanState.arrowCurrentBatchOffset;

        auto* dstColArray = batch.children[0];
        auto* dstColSchema = indicesSchema.children[0];
        common::ValueVector dstOffsetValueVector = common::ValueVector(LogicalType::UINT64(),
            memoryManager, DataChunkState::getSingleValueDataChunkState());

        ArrowUtils::readArrowValues(dstColSchema, dstColArray, *relScanState.arrowDstKeyVector,
            dstColArray->offset + relScanState.arrowCurrentBatchOffset, 0, 1);

        if (dstOffsetValueVector.isNull(0)) {
            relScanState.arrowCurrentBatchOffset++;
            continue;
        }

        const auto srcNodeOffset = findSourceNodeForRow(relOffset);
        const auto dstNodeOffset = dstOffsetValueVector.getValue<offset_t>(0);

        if (srcNodeOffset == INVALID_OFFSET || dstNodeOffset == INVALID_OFFSET) {
            relScanState.arrowCurrentBatchOffset++;
            continue;
        }

        auto boundOffset = isFwd ? srcNodeOffset : dstNodeOffset;
        auto boundIt = relScanState.arrowBoundNodeOffsetToSelPos.find(boundOffset);

        if (boundIt == relScanState.arrowBoundNodeOffsetToSelPos.end()) {
            relScanState.arrowCurrentBatchOffset++;
            continue;
        }

        if (!hasActiveBound) {
            hasActiveBound = true;
            activeBoundOffset = boundOffset;
            activeBoundSelPos = boundIt->second;
        } else if (boundOffset != activeBoundOffset) {
            break;
        }

        auto nbrOffset = isFwd ? dstNodeOffset : srcNodeOffset;
        auto nbrTableID = isFwd ? getToNodeTableID() : getFromNodeTableID();

        if (!relScanState.outputVectors.empty()) {
            relScanState.outputVectors[0]->setValue<internalID_t>(outputCount,
                internalID_t{nbrOffset, nbrTableID});
        }

        for (uint64_t outCol = 1; outCol < relScanState.outputVectors.size(); ++outCol) {
            if (!relScanState.outputVectors[outCol]) {
                continue;
            }

            auto colID = scanState.columnIDs[outCol];

            if (colID == REL_ID_COLUMN_ID) {
                relScanState.outputVectors[outCol]->setValue<internalID_t>(outputCount,
                    internalID_t{relOffset, getTableID()});
                continue;
            }

            if (!propertyColumnToArrowColumnIdx.contains(colID)) {
                continue;
            }

            auto arrowColIdx = propertyColumnToArrowColumnIdx[colID];

            if (arrowColIdx < 0 ||
                static_cast<uint64_t>(arrowColIdx) >= static_cast<uint64_t>(batch.n_children) ||
                !batch.children[arrowColIdx] || !indicesSchema.children[arrowColIdx]) {
                continue;
            }

            auto* childArray = batch.children[arrowColIdx];
            auto* childSchema = indicesSchema.children[arrowColIdx];
            ArrowUtils::readArrowValues(childSchema, childArray,
                *relScanState.outputVectors[outCol],
                childArray->offset + relScanState.arrowCurrentBatchOffset, outputCount, 1);
        }

        outputCount++;
        relScanState.arrowCurrentBatchOffset++;
    }

    if (outputCount == 0) {
        relScanState.outState->getSelVectorUnsafe().setToFiltered(0);
        return false;
    }

    auto& selVector = relScanState.outState->getSelVectorUnsafe();
    selVector.setToUnfiltered(outputCount);
    relScanState.setNodeIDVectorToFlat(activeBoundSelPos);
    relScanState.arrowScanCompleted = relScanState.arrowCurrentBatchIdx >= indices.size();

    return true;
}

offset_t IceMemRelTable::findSourceNodeForRow(uint64_t globalRowOffset) const {
    // read each batch in indptr and find globalRowOffset in it. Note: indptr is sorted
    offset_t currentBatchStartOffset = 0;

    for (size_t batchIdx = 0; batchIdx < indptr.size(); ++batchIdx) {
        const auto& batch = indptr[batchIdx];
        auto batchLength = ArrowUtils::getArrowBatchLength(batch);

        if (batchLength == 0 || !batch.children || batch.n_children <= 0 || !batch.children[0]) {
            continue;
        }

        auto* indptrColArray = batch.children[0];
        auto* indptrColSchema = indptrSchema.children[0];

        auto low = 0;
        auto high = batchLength - 1;

        common::ValueVector lowValueVector = common::ValueVector(LogicalType::UINT64(),
            memoryManager, DataChunkState::getSingleValueDataChunkState());
        ArrowUtils::readArrowValues(indptrColSchema, indptrColArray, lowValueVector,
            indptrColArray->offset + low, 0, 1);

        if (lowValueVector.isNull(0)) {
            throw RuntimeException("icebug-memory rel table's indptr table contains null values, "
                                   "which is not allowed");
        }

        auto lowValue = lowValueVector.getValue<offset_t>(0);

        if (globalRowOffset <= lowValue) {
            if (currentBatchStartOffset == 0) {
                return INVALID_OFFSET;
            } else {
                return currentBatchStartOffset - 1;
            }
        }

        common::ValueVector highValueVector = common::ValueVector(LogicalType::UINT64(),
            memoryManager, DataChunkState::getSingleValueDataChunkState());
        ArrowUtils::readArrowValues(indptrColSchema, indptrColArray, highValueVector,
            indptrColArray->offset + high, 0, 1);

        if (highValueVector.isNull(0)) {
            throw RuntimeException("icebug-memory rel table's indptr table contains null values, "
                                   "which is not allowed");
        }

        auto highValue = highValueVector.getValue<offset_t>(0);

        if (globalRowOffset > highValue) {
            currentBatchStartOffset += batchLength;
            continue;
        }

        while (high - low > 1) {
            auto mid = low + (high - low) / 2;
            common::ValueVector currValueVector = common::ValueVector(LogicalType::UINT64(),
                memoryManager, DataChunkState::getSingleValueDataChunkState());
            ArrowUtils::readArrowValues(indptrColSchema, indptrColArray, currValueVector,
                indptrColArray->offset + mid, 0, 1);

            if (currValueVector.isNull(0)) {
                throw RuntimeException("icebug-memory rel table's indptr table contains null "
                                       "values, which is not allowed");
            }

            auto midValue = currValueVector.getValue<offset_t>(0);

            if (globalRowOffset <= midValue) {
                high = mid;
            } else if (globalRowOffset > midValue) {
                low = mid;
            }
        }

        return batchStartOffsets[batchIdx] + low;
    }

    return INVALID_OFFSET;
}

row_idx_t IceMemRelTable::getTotalRowCount(
    [[maybe_unused]] const transaction::Transaction* transaction) const {
    return totalIndicesRows;
}

} // namespace storage
} // namespace lbug

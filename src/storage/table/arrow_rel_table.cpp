#include "storage/table/arrow_rel_table.h"

#include <cstring>
#include <queue>

#include "common/arrow/arrow_converter.h"
#include "common/arrow/arrow_nullmask_tree.h"
#include "common/data_chunk/sel_vector.h"
#include "common/exception/runtime.h"
#include "common/system_config.h"
#include "common/types/internal_id_util.h"
#include "storage/table/arrow_table_support.h"
#include "storage/table/csr_node_group.h"
#include "transaction/transaction.h"

namespace lbug {
namespace storage {

using namespace common;

static uint64_t getArrowBatchLength(const ArrowArrayWrapper& array) {
    if (array.length > 0) {
        return array.length;
    }
    if (array.n_children > 0 && array.children && array.children[0]) {
        return array.children[0]->length;
    }
    return 0;
}

static int64_t findColumnIdx(const ArrowSchemaWrapper& schema, const std::string& colName) {
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (schema.children && schema.children[i] && schema.children[i]->name &&
            colName == schema.children[i]->name) {
            return i;
        }
    }
    return -1;
}

void ArrowRelTableScanState::setToTable(const transaction::Transaction* transaction, Table* table_,
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

ArrowRelTable::ArrowRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, table_id_t fromTableID,
    table_id_t toTableID, const StorageManager* storageManager, MemoryManager* memoryManager,
    const NodeTable* fromNodeTable, const NodeTable* toNodeTable, ArrowRelTableLayout layout,
    ArrowSchemaWrapper schema, std::vector<ArrowArrayWrapper> arrays,
    ArrowSchemaWrapper indptrSchema, std::vector<ArrowArrayWrapper> indptrArrays,
    std::string arrowId, std::string dstColumnName)
    : ColumnarRelTableBase{relGroupEntry, fromTableID, toTableID, storageManager, memoryManager},
      fromNodeTable{fromNodeTable}, toNodeTable{toNodeTable}, layout{layout},
      schema{std::move(schema)}, arrays{std::move(arrays)}, indptrSchema{std::move(indptrSchema)},
      indptrArrays{std::move(indptrArrays)}, arrowId{std::move(arrowId)} {
    if (!this->schema.format) {
        throw RuntimeException("Arrow schema format cannot be null");
    }
    if (!this->fromNodeTable || !this->toNodeTable) {
        throw RuntimeException(
            "Arrow relationship table requires source and destination node tables");
    }

    if (this->layout == ArrowRelTableLayout::FLAT) {
        fromColumnIdx = findColumnIdx(this->schema, "from");
        toColumnIdx = findColumnIdx(this->schema, "to");
        if (fromColumnIdx < 0 || toColumnIdx < 0) {
            throw RuntimeException(
                "Arrow FLAT relationship table requires 'from' and 'to' columns");
        }

        auto srcArrowType = ArrowConverter::fromArrowSchema(this->schema.children[fromColumnIdx]);
        auto dstArrowType = ArrowConverter::fromArrowSchema(this->schema.children[toColumnIdx]);
        const auto& srcPKType =
            this->fromNodeTable->getColumn(this->fromNodeTable->getPKColumnID()).getDataType();
        const auto& dstPKType =
            this->toNodeTable->getColumn(this->toNodeTable->getPKColumnID()).getDataType();
        if (srcArrowType.toString() != srcPKType.toString()) {
            throw RuntimeException("Arrow 'from' column type " + srcArrowType.toString() +
                                   " must match source node PK type " + srcPKType.toString());
        }
        if (dstArrowType.toString() != dstPKType.toString()) {
            throw RuntimeException("Arrow 'to' column type " + dstArrowType.toString() +
                                   " must match destination node PK type " + dstPKType.toString());
        }
    } else {
        csrNbrColumnIdx = findColumnIdx(this->schema, dstColumnName);
        if (csrNbrColumnIdx < 0) {
            throw RuntimeException(
                "Arrow CSR relationship table requires a '" + dstColumnName + "' column");
        }
        auto nbrArrowType = ArrowConverter::fromArrowSchema(this->schema.children[csrNbrColumnIdx]);
        if (nbrArrowType.getLogicalTypeID() != LogicalTypeID::UINT64) {
            throw RuntimeException("Arrow CSR '" + dstColumnName + "' column type " +
                                   nbrArrowType.toString() + " must be UINT64 node offsets");
        }
        if (!this->indptrSchema.format || this->indptrArrays.empty()) {
            throw RuntimeException("Arrow CSR relationship table requires an indptr Arrow table");
        }
        if (this->indptrSchema.n_children <= 0 || !this->indptrSchema.children ||
            !this->indptrSchema.children[0]) {
            throw RuntimeException("Arrow CSR indptr table requires one offset column");
        }
        auto indptrArrowType = ArrowConverter::fromArrowSchema(this->indptrSchema.children[0]);
        if (indptrArrowType.getLogicalTypeID() != LogicalTypeID::UINT64) {
            throw RuntimeException("Arrow CSR indptr column type " + indptrArrowType.toString() +
                                   " must be UINT64 offsets");
        }
    }

    for (const auto& prop : relGroupEntry->getProperties()) {
        if (prop.getName() == "_ID") {
            continue;
        }
        auto columnID = relGroupEntry->getColumnID(prop.getName());
        if (columnID == NBR_ID_COLUMN_ID || columnID == REL_ID_COLUMN_ID) {
            continue;
        }
        auto arrowColIdx = findColumnIdx(this->schema, prop.getName());
        if (arrowColIdx < 0) {
            throw RuntimeException(
                "Missing property column '" + prop.getName() + "' in Arrow relationship data");
        }
        propertyColumnToArrowColumnIdx[columnID] = arrowColIdx;
    }

    for (const auto& array : this->arrays) {
        batchStartOffsets.push_back(totalRows);
        totalRows += getArrowBatchLength(array);
    }
    for (const auto& array : this->indptrArrays) {
        indptrBatchStartOffsets.push_back(totalIndptrRows);
        totalIndptrRows += getArrowBatchLength(array);
    }
}

ArrowRelTable::~ArrowRelTable() {
    if (!arrowId.empty()) {
        ArrowTableSupport::unregisterArrowData(arrowId);
    }
}

void ArrowRelTable::initScanState([[maybe_unused]] transaction::Transaction* transaction,
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
    relScanState.arrowCSRBoundIdx = 0;
    relScanState.arrowCSRCurrentRelOffset = INVALID_OFFSET;
    relScanState.arrowScanCompleted = arrays.empty();

    auto singleValueState = DataChunkState::getSingleValueDataChunkState();
    if (layout == ArrowRelTableLayout::FLAT) {
        auto srcPKType =
            fromNodeTable->getColumn(fromNodeTable->getPKColumnID()).getDataType().copy();
        auto dstPKType = toNodeTable->getColumn(toNodeTable->getPKColumnID()).getDataType().copy();
        relScanState.arrowSrcKeyVector =
            std::make_unique<ValueVector>(std::move(srcPKType), memoryManager, singleValueState);
        relScanState.arrowDstKeyVector =
            std::make_unique<ValueVector>(std::move(dstPKType), memoryManager, singleValueState);
    } else {
        relScanState.arrowSrcKeyVector =
            std::make_unique<ValueVector>(LogicalType::UINT64(), memoryManager, singleValueState);
        relScanState.arrowDstKeyVector =
            std::make_unique<ValueVector>(LogicalType::UINT64(), memoryManager, singleValueState);
    }
    relScanState.arrowSrcKeyVector->state->setToFlat();
    relScanState.arrowDstKeyVector->state->setToFlat();
}

static void readSingleArrowValue(const ArrowSchema* schema, const ArrowArray* array,
    ValueVector& outputVector, uint64_t srcOffset, uint64_t dstOffset) {
    ArrowNullMaskTree nullMask(schema, array, array->offset, array->length);
    ArrowConverter::fromArrowArray(schema, array, outputVector, &nullMask, srcOffset, dstOffset, 1);
}

bool ArrowRelTable::scanInternal(transaction::Transaction* transaction, TableScanState& scanState) {
    if (layout == ArrowRelTableLayout::CSR) {
        return scanCSR(scanState);
    }
    return scanFlat(transaction, scanState);
}

bool ArrowRelTable::scanFlat(transaction::Transaction* transaction, TableScanState& scanState) {
    auto& relScanState = scanState.cast<RelTableScanState>();
    if (relScanState.arrowScanCompleted || !relScanState.arrowSrcKeyVector ||
        !relScanState.arrowDstKeyVector) {
        return false;
    }

    scanState.resetOutVectors();
    auto outputCount = 0u;
    constexpr uint64_t maxRowsPerCall = DEFAULT_VECTOR_CAPACITY;
    auto activeBoundSelPos = INVALID_SEL;
    auto activeBoundOffset = INVALID_OFFSET;
    auto hasActiveBound = false;
    const auto outputToArrowColumnIdx = getOutputToArrowColumnIdx(scanState.columnIDs);

    while (outputCount < maxRowsPerCall && relScanState.arrowCurrentBatchIdx < arrays.size()) {
        const auto& batch = arrays[relScanState.arrowCurrentBatchIdx];
        auto batchLength = getArrowBatchLength(batch);
        if (relScanState.arrowCurrentBatchOffset >= batchLength) {
            relScanState.arrowCurrentBatchIdx++;
            relScanState.arrowCurrentBatchOffset = 0;
            continue;
        }

        auto srcOffsetInBatch = relScanState.arrowCurrentBatchOffset;
        auto numChildren = batch.n_children < 0 ? 0u : static_cast<uint64_t>(batch.n_children);
        if (numChildren == 0 || !batch.children || !schema.children ||
            static_cast<uint64_t>(fromColumnIdx) >= numChildren ||
            static_cast<uint64_t>(toColumnIdx) >= numChildren || !batch.children[fromColumnIdx] ||
            !batch.children[toColumnIdx] || !schema.children[fromColumnIdx] ||
            !schema.children[toColumnIdx]) {
            relScanState.arrowCurrentBatchOffset++;
            continue;
        }

        auto* srcChildArray = batch.children[fromColumnIdx];
        auto* srcChildSchema = schema.children[fromColumnIdx];
        auto* dstChildArray = batch.children[toColumnIdx];
        auto* dstChildSchema = schema.children[toColumnIdx];
        auto srcOffsetToRead = srcChildArray->offset + srcOffsetInBatch;
        auto dstOffsetToRead = dstChildArray->offset + srcOffsetInBatch;
        readSingleArrowValue(srcChildSchema, srcChildArray, *relScanState.arrowSrcKeyVector,
            srcOffsetToRead, 0);
        if (relScanState.arrowSrcKeyVector->isNull(0)) {
            relScanState.arrowCurrentBatchOffset++;
            continue;
        }
        readSingleArrowValue(dstChildSchema, dstChildArray, *relScanState.arrowDstKeyVector,
            dstOffsetToRead, 0);
        if (relScanState.arrowDstKeyVector->isNull(0)) {
            relScanState.arrowCurrentBatchOffset++;
            continue;
        }

        offset_t srcNodeOffset = INVALID_OFFSET;
        offset_t dstNodeOffset = INVALID_OFFSET;
        if (!fromNodeTable->lookupPK(transaction, relScanState.arrowSrcKeyVector.get(), 0,
                srcNodeOffset)) {
            relScanState.arrowCurrentBatchOffset++;
            continue;
        }
        if (!toNodeTable->lookupPK(transaction, relScanState.arrowDstKeyVector.get(), 0,
                dstNodeOffset)) {
            relScanState.arrowCurrentBatchOffset++;
            continue;
        }

        auto isFwd = relScanState.direction != RelDataDirection::BWD;
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
        auto relOffset = batchStartOffsets[relScanState.arrowCurrentBatchIdx] + srcOffsetInBatch;
        if (!relScanState.outputVectors.empty()) {
            relScanState.outputVectors[0]->setValue<internalID_t>(outputCount,
                internalID_t{nbrOffset, nbrTableID});
        }

        for (uint64_t outCol = 1; outCol < relScanState.outputVectors.size(); ++outCol) {
            if (!relScanState.outputVectors[outCol]) {
                continue;
            }
            if (outCol < scanState.columnIDs.size() &&
                scanState.columnIDs[outCol] == REL_ID_COLUMN_ID) {
                relScanState.outputVectors[outCol]->setValue<internalID_t>(outputCount,
                    internalID_t{relOffset, getTableID()});
                continue;
            }
            if (outCol >= outputToArrowColumnIdx.size()) {
                continue;
            }
            auto arrowColIdx = outputToArrowColumnIdx[outCol];
            if (arrowColIdx < 0 || static_cast<uint64_t>(arrowColIdx) >= numChildren ||
                !batch.children[arrowColIdx] || !schema.children[arrowColIdx]) {
                continue;
            }
            auto* childArray = batch.children[arrowColIdx];
            auto* childSchema = schema.children[arrowColIdx];
            readSingleArrowValue(childSchema, childArray, *relScanState.outputVectors[outCol],
                childArray->offset + srcOffsetInBatch, outputCount);
        }
        outputCount++;
        relScanState.arrowCurrentBatchOffset++;
    }

    if (outputCount == 0) {
        relScanState.arrowScanCompleted = relScanState.arrowCurrentBatchIdx >= arrays.size();
        relScanState.outState->getSelVectorUnsafe().setToFiltered(0);
        return false;
    }

    relScanState.setNodeIDVectorToFlat(activeBoundSelPos);
    auto& selVector = relScanState.outState->getSelVectorUnsafe();
    selVector.setToFiltered(outputCount);
    for (uint64_t i = 0; i < outputCount; ++i) {
        selVector[i] = i;
    }
    relScanState.arrowScanCompleted = relScanState.arrowCurrentBatchIdx >= arrays.size();
    return true;
}

bool ArrowRelTable::scanCSR(TableScanState& scanState) {
    auto& relScanState = scanState.cast<RelTableScanState>();
    if (relScanState.arrowScanCompleted || !relScanState.arrowSrcKeyVector ||
        !relScanState.arrowDstKeyVector) {
        return false;
    }

    scanState.resetOutVectors();
    auto outputCount = 0u;
    constexpr uint64_t maxRowsPerCall = DEFAULT_VECTOR_CAPACITY;
    auto activeBoundSelPos = INVALID_SEL;
    auto activeBoundOffset = INVALID_OFFSET;
    auto hasActiveBound = false;
    const auto outputToArrowColumnIdx = getOutputToArrowColumnIdx(scanState.columnIDs);
    const auto isFwd = relScanState.direction != RelDataDirection::BWD;

    if (isFwd) {
        while (outputCount < maxRowsPerCall &&
               relScanState.arrowCSRBoundIdx < relScanState.cachedBoundNodeSelVector.getSelSize()) {
            auto boundNodeIdx =
                relScanState.cachedBoundNodeSelVector[relScanState.arrowCSRBoundIdx];
            const auto boundNodeID = relScanState.nodeIDVector->getValue<nodeID_t>(boundNodeIdx);
            offset_t startOffset = INVALID_OFFSET;
            offset_t endOffset = INVALID_OFFSET;
            if (!readIndptr(boundNodeID.offset, startOffset) ||
                !readIndptr(boundNodeID.offset + 1, endOffset) || startOffset > endOffset) {
                relScanState.arrowCSRBoundIdx++;
                relScanState.arrowCSRCurrentRelOffset = INVALID_OFFSET;
                continue;
            }
            if (relScanState.arrowCSRCurrentRelOffset == INVALID_OFFSET) {
                relScanState.arrowCSRCurrentRelOffset = startOffset;
            }
            if (relScanState.arrowCSRCurrentRelOffset >= endOffset) {
                relScanState.arrowCSRBoundIdx++;
                relScanState.arrowCSRCurrentRelOffset = INVALID_OFFSET;
                continue;
            }

            if (!hasActiveBound) {
                hasActiveBound = true;
                activeBoundOffset = boundNodeID.offset;
                activeBoundSelPos = boundNodeIdx;
            } else if (boundNodeID.offset != activeBoundOffset) {
                break;
            }

            if (!readCSRValue(*relScanState.arrowDstKeyVector,
                    relScanState.arrowCSRCurrentRelOffset, 0) ||
                relScanState.arrowDstKeyVector->isNull(0)) {
                relScanState.arrowCSRCurrentRelOffset++;
                continue;
            }
            auto nbrOffset = relScanState.arrowDstKeyVector->getValue<offset_t>(0);
            auto relOffset = relScanState.arrowCSRCurrentRelOffset;
            if (!relScanState.outputVectors.empty()) {
                relScanState.outputVectors[0]->setValue<internalID_t>(outputCount,
                    internalID_t{nbrOffset, getToNodeTableID()});
            }
            for (uint64_t outCol = 1; outCol < relScanState.outputVectors.size(); ++outCol) {
                if (!relScanState.outputVectors[outCol]) {
                    continue;
                }
                if (outCol < scanState.columnIDs.size() &&
                    scanState.columnIDs[outCol] == REL_ID_COLUMN_ID) {
                    relScanState.outputVectors[outCol]->setValue<internalID_t>(outputCount,
                        internalID_t{relOffset, getTableID()});
                    continue;
                }
                if (outCol >= outputToArrowColumnIdx.size() || outputToArrowColumnIdx[outCol] < 0) {
                    continue;
                }
                readArrowValueAtOffset(schema, arrays, batchStartOffsets,
                    outputToArrowColumnIdx[outCol], relOffset, *relScanState.outputVectors[outCol],
                    outputCount);
            }
            outputCount++;
            relScanState.arrowCSRCurrentRelOffset++;
        }
    } else {
        while (outputCount < maxRowsPerCall && relScanState.arrowCurrentBatchOffset < totalRows) {
            auto relOffset = relScanState.arrowCurrentBatchOffset;
            if (!readCSRValue(*relScanState.arrowDstKeyVector, relOffset, 0) ||
                relScanState.arrowDstKeyVector->isNull(0)) {
                relScanState.arrowCurrentBatchOffset++;
                continue;
            }
            auto dstOffset = relScanState.arrowDstKeyVector->getValue<offset_t>(0);
            auto boundIt = relScanState.arrowBoundNodeOffsetToSelPos.find(dstOffset);
            if (boundIt == relScanState.arrowBoundNodeOffsetToSelPos.end()) {
                relScanState.arrowCurrentBatchOffset++;
                continue;
            }
            if (!hasActiveBound) {
                hasActiveBound = true;
                activeBoundOffset = dstOffset;
                activeBoundSelPos = boundIt->second;
            } else if (dstOffset != activeBoundOffset) {
                break;
            }
            auto srcOffset = findCSRSourceOffset(relOffset);
            if (srcOffset == INVALID_OFFSET) {
                relScanState.arrowCurrentBatchOffset++;
                continue;
            }
            if (!relScanState.outputVectors.empty()) {
                relScanState.outputVectors[0]->setValue<internalID_t>(outputCount,
                    internalID_t{srcOffset, getFromNodeTableID()});
            }
            for (uint64_t outCol = 1; outCol < relScanState.outputVectors.size(); ++outCol) {
                if (!relScanState.outputVectors[outCol]) {
                    continue;
                }
                if (outCol < scanState.columnIDs.size() &&
                    scanState.columnIDs[outCol] == REL_ID_COLUMN_ID) {
                    relScanState.outputVectors[outCol]->setValue<internalID_t>(outputCount,
                        internalID_t{relOffset, getTableID()});
                    continue;
                }
                if (outCol >= outputToArrowColumnIdx.size() || outputToArrowColumnIdx[outCol] < 0) {
                    continue;
                }
                readArrowValueAtOffset(schema, arrays, batchStartOffsets,
                    outputToArrowColumnIdx[outCol], relOffset, *relScanState.outputVectors[outCol],
                    outputCount);
            }
            outputCount++;
            relScanState.arrowCurrentBatchOffset++;
        }
    }

    if (outputCount == 0) {
        relScanState.arrowScanCompleted =
            isFwd ? relScanState.arrowCSRBoundIdx >=
                        relScanState.cachedBoundNodeSelVector.getSelSize() :
                    relScanState.arrowCurrentBatchOffset >= totalRows;
        relScanState.outState->getSelVectorUnsafe().setToFiltered(0);
        return false;
    }

    relScanState.setNodeIDVectorToFlat(activeBoundSelPos);
    auto& selVector = relScanState.outState->getSelVectorUnsafe();
    selVector.setToFiltered(outputCount);
    for (uint64_t i = 0; i < outputCount; ++i) {
        selVector[i] = i;
    }
    relScanState.arrowScanCompleted =
        isFwd ?
            relScanState.arrowCSRBoundIdx >= relScanState.cachedBoundNodeSelVector.getSelSize() :
            relScanState.arrowCurrentBatchOffset >= totalRows;
    return true;
}

bool ArrowRelTable::readCSRValue(ValueVector& outputVector, offset_t relOffset,
    uint64_t dstOffset) const {
    return readArrowValueAtOffset(schema, arrays, batchStartOffsets, csrNbrColumnIdx, relOffset,
        outputVector, dstOffset);
}

bool ArrowRelTable::readIndptr(offset_t srcOffset, offset_t& result) const {
    auto singleValueState = DataChunkState::getSingleValueDataChunkState();
    ValueVector valueVector{LogicalType::UINT64(), memoryManager, singleValueState};
    valueVector.state->setToFlat();
    if (!readArrowValueAtOffset(indptrSchema, indptrArrays, indptrBatchStartOffsets,
            csrIndptrColumnIdx, srcOffset, valueVector, 0) ||
        valueVector.isNull(0)) {
        return false;
    }
    result = valueVector.getValue<offset_t>(0);
    return true;
}

offset_t ArrowRelTable::findCSRSourceOffset(offset_t relOffset) const {
    if (totalIndptrRows < 2) {
        return INVALID_OFFSET;
    }
    offset_t low = 0;
    offset_t high = totalIndptrRows - 1;
    while (low + 1 < high) {
        const auto mid = low + (high - low) / 2;
        offset_t midValue = INVALID_OFFSET;
        if (!readIndptr(mid, midValue)) {
            return INVALID_OFFSET;
        }
        if (relOffset < midValue) {
            high = mid;
        } else {
            low = mid;
        }
    }
    offset_t start = INVALID_OFFSET;
    offset_t end = INVALID_OFFSET;
    if (!readIndptr(low, start) || !readIndptr(low + 1, end) || relOffset < start ||
        relOffset >= end) {
        return INVALID_OFFSET;
    }
    return low;
}

bool ArrowRelTable::readArrowValueAtOffset(const ArrowSchemaWrapper& arrowSchema,
    const std::vector<ArrowArrayWrapper>& arrowArrays, const std::vector<size_t>& startOffsets,
    int64_t columnIdx, offset_t rowOffset, ValueVector& outputVector, uint64_t dstOffset) const {
    if (columnIdx < 0 || arrowArrays.empty() || startOffsets.size() != arrowArrays.size()) {
        return false;
    }
    for (size_t batchIdx = 0; batchIdx < arrowArrays.size(); ++batchIdx) {
        const auto& batch = arrowArrays[batchIdx];
        auto batchLength = getArrowBatchLength(batch);
        auto batchStart = startOffsets[batchIdx];
        if (rowOffset < batchStart || rowOffset >= batchStart + batchLength) {
            continue;
        }
        auto rowInBatch = rowOffset - batchStart;
        auto numChildren = batch.n_children < 0 ? 0u : static_cast<uint64_t>(batch.n_children);
        if (static_cast<uint64_t>(columnIdx) >= numChildren || !batch.children ||
            !arrowSchema.children || !batch.children[columnIdx] ||
            !arrowSchema.children[columnIdx]) {
            return false;
        }
        auto* childArray = batch.children[columnIdx];
        auto* childSchema = arrowSchema.children[columnIdx];
        readSingleArrowValue(childSchema, childArray, outputVector, childArray->offset + rowInBatch,
            dstOffset);
        return true;
    }
    return false;
}

std::vector<int64_t> ArrowRelTable::getOutputToArrowColumnIdx(
    const std::vector<column_id_t>& columnIDs) const {
    std::vector<int64_t> outputToArrowColumnIdx(columnIDs.size(), -1);
    for (size_t outCol = 0; outCol < columnIDs.size(); ++outCol) {
        auto columnID = columnIDs[outCol];
        if (columnID == NBR_ID_COLUMN_ID || columnID == INVALID_COLUMN_ID ||
            columnID == ROW_IDX_COLUMN_ID) {
            continue;
        }
        if (propertyColumnToArrowColumnIdx.contains(columnID)) {
            outputToArrowColumnIdx[outCol] = propertyColumnToArrowColumnIdx.at(columnID);
        }
    }
    return outputToArrowColumnIdx;
}

row_idx_t ArrowRelTable::getTotalRowCount(
    [[maybe_unused]] const transaction::Transaction* transaction) const {
    return totalRows;
}

row_idx_t ArrowRelTable::getActiveBoundNodeCount(
    [[maybe_unused]] const transaction::Transaction* transaction,
    RelDataDirection direction) const {
    if (layout != ArrowRelTableLayout::CSR || direction == RelDataDirection::BWD) {
        return 0;
    }
    row_idx_t result = 0;
    for (offset_t i = 0; i + 1 < totalIndptrRows; ++i) {
        offset_t start = INVALID_OFFSET;
        offset_t end = INVALID_OFFSET;
        if (readIndptr(i, start) && readIndptr(i + 1, end) && end > start) {
            result++;
        }
    }
    return result;
}

std::vector<std::pair<offset_t, row_idx_t>> ArrowRelTable::getAllDegreeEntries(
    [[maybe_unused]] const transaction::Transaction* transaction,
    RelDataDirection direction) const {
    if (layout != ArrowRelTableLayout::CSR || direction == RelDataDirection::BWD) {
        return {};
    }
    std::vector<std::pair<offset_t, row_idx_t>> result;
    result.reserve(totalIndptrRows);
    for (offset_t i = 0; i + 1 < totalIndptrRows; ++i) {
        offset_t start = INVALID_OFFSET;
        offset_t end = INVALID_OFFSET;
        if (!readIndptr(i, start) || !readIndptr(i + 1, end) || end <= start) {
            continue;
        }
        result.emplace_back(i, end - start);
    }
    return result;
}

std::vector<std::pair<offset_t, row_idx_t>> ArrowRelTable::getTopKDegreeEntries(
    [[maybe_unused]] const transaction::Transaction* transaction, RelDataDirection direction,
    idx_t k) const {
    if (layout != ArrowRelTableLayout::CSR || direction == RelDataDirection::BWD || k == 0) {
        return {};
    }
    using degree_entry_t = std::pair<offset_t, row_idx_t>;
    auto better = [](const degree_entry_t& a, const degree_entry_t& b) {
        return a.second > b.second || (a.second == b.second && a.first < b.first);
    };
    auto worseForHeap = [better](const degree_entry_t& a, const degree_entry_t& b) {
        return better(a, b);
    };
    std::priority_queue<degree_entry_t, std::vector<degree_entry_t>, decltype(worseForHeap)> heap{
        worseForHeap};
    for (offset_t i = 0; i + 1 < totalIndptrRows; ++i) {
        offset_t start = INVALID_OFFSET;
        offset_t end = INVALID_OFFSET;
        if (!readIndptr(i, start) || !readIndptr(i + 1, end) || end <= start) {
            continue;
        }
        degree_entry_t entry{i, end - start};
        if (heap.size() < k) {
            heap.push(entry);
        } else if (better(entry, heap.top())) {
            heap.pop();
            heap.push(entry);
        }
    }
    std::vector<degree_entry_t> result;
    while (!heap.empty()) {
        result.push_back(heap.top());
        heap.pop();
    }
    std::sort(result.begin(), result.end(), better);
    return result;
}

} // namespace storage
} // namespace lbug

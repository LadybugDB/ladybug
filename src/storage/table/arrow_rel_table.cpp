#include "storage/table/arrow_rel_table.h"

#include <cstring>

#include "common/arrow/arrow_converter.h"
#include "common/arrow/arrow_nullmask_tree.h"
#include "common/data_chunk/sel_vector.h"
#include "common/exception/runtime.h"
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
    // Same behavior as ParquetRelTable: no local table for external data sources.
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
    const NodeTable* fromNodeTable, const NodeTable* toNodeTable, ArrowSchemaWrapper schema,
    std::vector<ArrowArrayWrapper> arrays, std::string arrowId)
    : ColumnarRelTableBase{relGroupEntry, fromTableID, toTableID, storageManager, memoryManager},
      fromNodeTable{fromNodeTable}, toNodeTable{toNodeTable}, schema{std::move(schema)},
      arrays{std::move(arrays)}, arrowId{std::move(arrowId)} {
    if (!this->schema.format) {
        throw RuntimeException("Arrow schema format cannot be null");
    }
    if (!this->fromNodeTable || !this->toNodeTable) {
        throw RuntimeException(
            "Arrow relationship table requires source and destination node tables");
    }

    fromColumnIdx = findColumnIdx(this->schema, "from");
    toColumnIdx = findColumnIdx(this->schema, "to");
    if (fromColumnIdx < 0 || toColumnIdx < 0) {
        throw RuntimeException("Arrow relationship table requires 'from' and 'to' columns");
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
}

ArrowRelTable::~ArrowRelTable() {
    if (!arrowId.empty()) {
        ArrowTableSupport::unregisterArrowData(arrowId);
    }
}

void ArrowRelTable::initScanState([[maybe_unused]] transaction::Transaction* transaction,
    TableScanState& scanState, bool resetCachedBoundNodeSelVec) const {
    auto& relScanState = scanState.cast<ArrowRelTableScanState>();
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

    relScanState.boundNodeOffsets.clear();
    for (uint64_t i = 0; i < relScanState.cachedBoundNodeSelVector.getSelSize(); ++i) {
        auto boundNodeIdx = relScanState.cachedBoundNodeSelVector[i];
        const auto boundNodeID = relScanState.nodeIDVector->getValue<nodeID_t>(boundNodeIdx);
        relScanState.boundNodeOffsets.insert(boundNodeID.offset);
    }

    relScanState.outputToArrowColumnIdx.assign(scanState.columnIDs.size(), -1);
    for (size_t outCol = 0; outCol < scanState.columnIDs.size(); ++outCol) {
        auto columnID = scanState.columnIDs[outCol];
        if (columnID == NBR_ID_COLUMN_ID || columnID == INVALID_COLUMN_ID ||
            columnID == ROW_IDX_COLUMN_ID) {
            continue;
        }
        if (propertyColumnToArrowColumnIdx.contains(columnID)) {
            relScanState.outputToArrowColumnIdx[outCol] =
                propertyColumnToArrowColumnIdx.at(columnID);
        }
    }

    relScanState.currentBatchIdx = 0;
    relScanState.currentBatchOffset = 0;
    relScanState.scanCompleted = arrays.empty();

    auto srcPKType = fromNodeTable->getColumn(fromNodeTable->getPKColumnID()).getDataType().copy();
    auto dstPKType = toNodeTable->getColumn(toNodeTable->getPKColumnID()).getDataType().copy();
    auto singleValueState = DataChunkState::getSingleValueDataChunkState();
    relScanState.srcKeyVector =
        std::make_unique<ValueVector>(std::move(srcPKType), memoryManager, singleValueState);
    relScanState.dstKeyVector =
        std::make_unique<ValueVector>(std::move(dstPKType), memoryManager, singleValueState);
    relScanState.srcKeyVector->state->setToFlat();
    relScanState.dstKeyVector->state->setToFlat();
}

static void readSingleArrowValue(const ArrowSchema* schema, const ArrowArray* array,
    ValueVector& outputVector, uint64_t srcOffset, uint64_t dstOffset) {
    ArrowNullMaskTree nullMask(schema, array, array->offset, array->length);
    ArrowConverter::fromArrowArray(schema, array, outputVector, &nullMask, srcOffset, dstOffset, 1);
}

bool ArrowRelTable::scanInternal(transaction::Transaction* transaction, TableScanState& scanState) {
    auto& relScanState = scanState.cast<ArrowRelTableScanState>();
    if (relScanState.scanCompleted || !relScanState.srcKeyVector || !relScanState.dstKeyVector) {
        return false;
    }

    scanState.resetOutVectors();
    auto outputCount = 0u;
    constexpr uint64_t maxRowsPerCall = 1;

    while (outputCount < maxRowsPerCall && relScanState.currentBatchIdx < arrays.size()) {
        const auto& batch = arrays[relScanState.currentBatchIdx];
        auto batchLength = getArrowBatchLength(batch);
        if (relScanState.currentBatchOffset >= batchLength) {
            relScanState.currentBatchIdx++;
            relScanState.currentBatchOffset = 0;
            continue;
        }

        auto srcOffsetInBatch = relScanState.currentBatchOffset++;
        auto numChildren = batch.n_children < 0 ? 0u : static_cast<uint64_t>(batch.n_children);
        if (numChildren == 0 || !batch.children || !schema.children ||
            static_cast<uint64_t>(fromColumnIdx) >= numChildren ||
            static_cast<uint64_t>(toColumnIdx) >= numChildren || !batch.children[fromColumnIdx] ||
            !batch.children[toColumnIdx] || !schema.children[fromColumnIdx] ||
            !schema.children[toColumnIdx]) {
            continue;
        }

        auto* srcChildArray = batch.children[fromColumnIdx];
        auto* srcChildSchema = schema.children[fromColumnIdx];
        auto* dstChildArray = batch.children[toColumnIdx];
        auto* dstChildSchema = schema.children[toColumnIdx];
        auto srcOffsetToRead = srcChildArray->offset + srcOffsetInBatch;
        auto dstOffsetToRead = dstChildArray->offset + srcOffsetInBatch;
        readSingleArrowValue(srcChildSchema, srcChildArray, *relScanState.srcKeyVector,
            srcOffsetToRead, 0);
        if (relScanState.srcKeyVector->isNull(0)) {
            continue;
        }
        readSingleArrowValue(dstChildSchema, dstChildArray, *relScanState.dstKeyVector,
            dstOffsetToRead, 0);
        if (relScanState.dstKeyVector->isNull(0)) {
            continue;
        }

        offset_t srcNodeOffset = INVALID_OFFSET;
        offset_t dstNodeOffset = INVALID_OFFSET;
        if (!fromNodeTable->lookupPK(transaction, relScanState.srcKeyVector.get(), 0,
                srcNodeOffset)) {
            continue;
        }
        if (!toNodeTable->lookupPK(transaction, relScanState.dstKeyVector.get(), 0,
                dstNodeOffset)) {
            continue;
        }

        auto isFwd = relScanState.direction != RelDataDirection::BWD;
        auto boundOffset = isFwd ? srcNodeOffset : dstNodeOffset;
        if (!relScanState.boundNodeOffsets.contains(boundOffset)) {
            continue;
        }

        auto nbrOffset = isFwd ? dstNodeOffset : srcNodeOffset;
        auto nbrTableID = isFwd ? getToNodeTableID() : getFromNodeTableID();
        auto relOffset = batchStartOffsets[relScanState.currentBatchIdx] + srcOffsetInBatch;
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
            if (outCol >= relScanState.outputToArrowColumnIdx.size()) {
                continue;
            }
            auto arrowColIdx = relScanState.outputToArrowColumnIdx[outCol];
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
    }

    if (outputCount == 0) {
        relScanState.scanCompleted = relScanState.currentBatchIdx >= arrays.size();
        auto selVector = std::make_shared<SelectionVector>(0);
        relScanState.outState->setSelVector(selVector);
        return false;
    }

    auto selVector = std::make_shared<SelectionVector>(outputCount);
    selVector->setToFiltered(outputCount);
    for (uint64_t i = 0; i < outputCount; ++i) {
        (*selVector)[i] = i;
    }
    relScanState.outState->setSelVector(selVector);
    relScanState.scanCompleted = relScanState.currentBatchIdx >= arrays.size();
    return true;
}

row_idx_t ArrowRelTable::getTotalRowCount(
    [[maybe_unused]] const transaction::Transaction* transaction) const {
    return totalRows;
}

} // namespace storage
} // namespace lbug

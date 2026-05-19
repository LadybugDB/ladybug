#include "storage/table/arrow_rel_table.h"

#include <algorithm>
#include <cstring>

#include "common/arrow/arrow_converter.h"
#include "common/data_chunk/sel_vector.h"
#include "common/exception/runtime.h"
#include "common/system_config.h"
#include "common/types/internal_id_util.h"
#include "storage/table/arrow_node_table.h"
#include "storage/table/arrow_table_support.h"
#include "storage/table/arrow_utils.h"
#include "storage/table/csr_node_group.h"
#include "transaction/transaction.h"

namespace lbug {
namespace storage {

using namespace common;
namespace {

// Per-batch metadata for Arrow indptr columns.
struct IndptrBatchMeta {
    std::vector<size_t> batchOffsets;
    size_t totalEntries = 0;
};

// Compute batch start offsets and total entry count from struct-array indptr batches.
// Each batch is a struct with child[0] = UINT64 row pointers.
// Throws RuntimeException if a non-empty child batch is missing its data buffer.
IndptrBatchMeta buildIndptrMeta(const std::vector<ArrowArrayWrapper>& batches) {
    IndptrBatchMeta meta;
    meta.batchOffsets.reserve(batches.size());
    size_t total = 0;
    for (const auto& batch : batches) {
        meta.batchOffsets.push_back(total);
        if (batch.n_children < 1 || !batch.children || !batch.children[0]) {
            continue;
        }
        const auto* col = batch.children[0];
        if (col->length > 0) {
            if (!col->buffers || !col->buffers[1]) {
                throw RuntimeException("Invalid CSR indptr Arrow array: missing data buffer");
            }
        }
        total += static_cast<size_t>(col->length);
    }
    meta.totalEntries = total;
    return meta;
}

// Build cumulative batch start offsets for index batches.
std::vector<size_t> computeBatchOffsets(const std::vector<ArrowArrayWrapper>& batches) {
    std::vector<size_t> offsets;
    size_t total = 0;
    for (const auto& batch : batches) {
        offsets.push_back(total);
        total += ArrowUtils::getArrowBatchLength(batch);
    }
    return offsets;
}

// Sum total rows across all batches.
size_t sumBatchLengths(const std::vector<ArrowArrayWrapper>& batches) {
    size_t total = 0;
    for (const auto& b : batches) {
        total += ArrowUtils::getArrowBatchLength(b);
    }
    return total;
}

} // namespace

void ArrowRelTableScanState::setToTable(const transaction::Transaction* transaction, Table* table_,
    std::vector<column_id_t> columnIDs_, std::vector<ColumnPredicateSet> columnPredicateSets_,
    RelDataDirection direction_) {
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
    csrCursor = std::nullopt;
}

ArrowRelTable::ArrowRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, table_id_t fromTableID,
    table_id_t toTableID, const StorageManager* storageManager, MemoryManager* memoryManager,
    const NodeTable* fromNodeTable_, const NodeTable* toNodeTable_, ArrowSchemaWrapper schema,
    std::vector<ArrowArrayWrapper> arrays, std::string arrowId_)
    : ColumnarRelTableBase{relGroupEntry, fromTableID, toTableID, storageManager, memoryManager},
      fromNodeTable{fromNodeTable_}, toNodeTable{toNodeTable_}, edgeListSchema{std::move(schema)},
      edgeListArrays{std::move(arrays)}, arrowId{std::move(arrowId_)},
      layout{ArrowRelLayout::EdgeList} {

    if (!edgeListSchema.format) {
        throw RuntimeException("Arrow schema format cannot be null");
    }

    if (!fromNodeTable || !toNodeTable) {
        throw RuntimeException(
            "Arrow relationship table requires source and destination node tables");
    }

    fromColumnIdx = ArrowUtils::findColumnIdx(edgeListSchema, "from");
    toColumnIdx = ArrowUtils::findColumnIdx(edgeListSchema, "to");
    if (fromColumnIdx < 0 || toColumnIdx < 0) {
        throw RuntimeException("Arrow relationship table requires 'from' and 'to' columns");
    }

    auto srcArrowType = ArrowConverter::fromArrowSchema(edgeListSchema.children[fromColumnIdx]);
    auto dstArrowType = ArrowConverter::fromArrowSchema(edgeListSchema.children[toColumnIdx]);
    const auto& srcPKType = fromNodeTable->getColumn(fromNodeTable->getPKColumnID()).getDataType();
    const auto& dstPKType = toNodeTable->getColumn(toNodeTable->getPKColumnID()).getDataType();

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

        auto arrowColIdx = ArrowUtils::findColumnIdx(edgeListSchema, prop.getName());
        if (arrowColIdx < 0) {
            throw RuntimeException(
                "Missing property column '" + prop.getName() + "' in Arrow relationship data");
        }

        propertyColumnToArrowColumnIdx[columnID] = arrowColIdx;
    }

    for (const auto& array : edgeListArrays) {
        edgeListBatchOffsets.push_back(totalRows);
        totalRows += ArrowUtils::getArrowBatchLength(array);
    }
}

ArrowRelTable::ArrowRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, table_id_t fromTableID,
    table_id_t toTableID, const StorageManager* storageManager, MemoryManager* memoryManager,
    const NodeTable* fromNodeTable_, const NodeTable* toNodeTable_, ArrowCsrRelData csrData,
    std::string arrowId_)
    : ColumnarRelTableBase{relGroupEntry, fromTableID, toTableID, storageManager, memoryManager},
      fromNodeTable{fromNodeTable_}, toNodeTable{toNodeTable_}, arrowId{std::move(arrowId_)},
      layout{ArrowRelLayout::Csr} {

    fwdIndicesSchema = std::move(csrData.fwd.indicesSchema);
    fwdIndices = std::move(csrData.fwd.indices);
    fwdIndptr = std::move(csrData.fwd.indptr);

    {
        auto meta = buildIndptrMeta(fwdIndptr);
        fwdIndptrBatchOffsets = std::move(meta.batchOffsets);
        fwdIndptrTotalEntries = meta.totalEntries;
    }

    fwdBatchOffsets = computeBatchOffsets(fwdIndices);
    totalRows = sumBatchLengths(fwdIndices);

    if (csrData.bwd.has_value()) {
        hasBwd = true;
        bwdIndicesSchema = std::move(csrData.bwd->indicesSchema);
        bwdIndices = std::move(csrData.bwd->indices);
        bwdIndptr = std::move(csrData.bwd->indptr);

        {
            auto meta = buildIndptrMeta(bwdIndptr);
            bwdIndptrBatchOffsets = std::move(meta.batchOffsets);
            bwdIndptrTotalEntries = meta.totalEntries;
        }

        bwdBatchOffsets = computeBatchOffsets(bwdIndices);
    }

    // Map catalog properties → child index in fwd indices struct (child[0] = dst offset, skip).
    for (const auto& prop : relGroupEntry->getProperties()) {
        if (prop.getName() == "_ID") {
            continue;
        }

        auto columnID = relGroupEntry->getColumnID(prop.getName());
        if (columnID == NBR_ID_COLUMN_ID || columnID == REL_ID_COLUMN_ID) {
            continue;
        }

        int64_t found = -1;
        for (int64_t i = 1; i < fwdIndicesSchema.n_children; ++i) {
            if (fwdIndicesSchema.children[i] && fwdIndicesSchema.children[i]->name &&
                prop.getName() == fwdIndicesSchema.children[i]->name) {
                found = i;
                break;
            }
        }

        if (found < 0) {
            throw RuntimeException(
                "Missing property column '" + prop.getName() + "' in CSR indices data");
        }

        propertyColumnToArrowColumnIdx[columnID] = found;
    }
}

ArrowRelTable::~ArrowRelTable() {
    if (!arrowId.empty()) {
        ArrowTableSupport::unregisterArrowData(arrowId);  // no-op for CSR
        ArrowTableSupport::unregisterCsrRelData(arrowId); // no-op for EdgeList
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
        auto idx = relScanState.cachedBoundNodeSelVector[i];
        const auto nodeID = relScanState.nodeIDVector->getValue<nodeID_t>(idx);
        relScanState.arrowBoundNodeOffsetToSelPos.emplace(nodeID.offset, idx);
    }

    if (layout == ArrowRelLayout::EdgeList) {
        initEdgeListScanState(relScanState);
    } else {
        initCsrScanState(relScanState);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// EdgeList scan helpers
// ─────────────────────────────────────────────────────────────────────────────

void ArrowRelTable::initEdgeListScanState(RelTableScanState& relScanState) const {
    relScanState.arrowCurrentBatchIdx = 0;
    relScanState.arrowCurrentBatchOffset = 0;
    relScanState.arrowScanCompleted = edgeListArrays.empty();

    auto srcPKType = fromNodeTable->getColumn(fromNodeTable->getPKColumnID()).getDataType().copy();
    auto dstPKType = toNodeTable->getColumn(toNodeTable->getPKColumnID()).getDataType().copy();
    auto singleState = DataChunkState::getSingleValueDataChunkState();
    relScanState.arrowSrcKeyVector =
        std::make_unique<ValueVector>(std::move(srcPKType), memoryManager, singleState);
    relScanState.arrowDstKeyVector =
        std::make_unique<ValueVector>(std::move(dstPKType), memoryManager, singleState);
    relScanState.arrowSrcKeyVector->state->setToFlat();
    relScanState.arrowDstKeyVector->state->setToFlat();
}

bool ArrowRelTable::readEdgeEndpoints(const transaction::Transaction* transaction,
    const ArrowArrayWrapper& batch, size_t rowInBatch, RelTableScanState& relScanState,
    offset_t& srcOffset, offset_t& dstOffset) const {
    auto numChildren = batch.n_children < 0 ? 0u : static_cast<uint64_t>(batch.n_children);
    if (numChildren == 0 || !batch.children ||
        static_cast<uint64_t>(fromColumnIdx) >= numChildren ||
        static_cast<uint64_t>(toColumnIdx) >= numChildren || !batch.children[fromColumnIdx] ||
        !batch.children[toColumnIdx] || !edgeListSchema.children[fromColumnIdx] ||
        !edgeListSchema.children[toColumnIdx]) {
        return false;
    }

    auto* srcCol = batch.children[fromColumnIdx];
    auto* dstCol = batch.children[toColumnIdx];
    ArrowUtils::readArrowValues(edgeListSchema.children[fromColumnIdx], srcCol,
        *relScanState.arrowSrcKeyVector, srcCol->offset + rowInBatch, 0, 1);

    if (relScanState.arrowSrcKeyVector->isNull(0)) {
        return false;
    }

    ArrowUtils::readArrowValues(edgeListSchema.children[toColumnIdx], dstCol,
        *relScanState.arrowDstKeyVector, dstCol->offset + rowInBatch, 0, 1);
    if (relScanState.arrowDstKeyVector->isNull(0)) {
        return false;
    }

    if (!fromNodeTable->lookupPK(transaction, relScanState.arrowSrcKeyVector.get(), 0, srcOffset)) {
        return false;
    }

    if (!toNodeTable->lookupPK(transaction, relScanState.arrowDstKeyVector.get(), 0, dstOffset)) {
        return false;
    }
    return true;
}

void ArrowRelTable::writeEdgeListRow(const ArrowArrayWrapper& batch, size_t rowInBatch,
    size_t globalEdgeIdx, offset_t nbrOffset, table_id_t nbrTableID, uint32_t outputCount,
    const std::vector<int64_t>& outputColumnIndices, TableScanState& scanState) const {
    auto& relScanState = scanState.cast<RelTableScanState>();
    auto numChildren = batch.n_children < 0 ? 0u : static_cast<uint64_t>(batch.n_children);

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
                internalID_t{static_cast<offset_t>(globalEdgeIdx), getTableID()});
            continue;
        }
        if (outCol >= outputColumnIndices.size()) {
            continue;
        }
        auto arrowColIdx = outputColumnIndices[outCol];
        if (arrowColIdx < 0 || static_cast<uint64_t>(arrowColIdx) >= numChildren ||
            !batch.children[arrowColIdx] || !edgeListSchema.children[arrowColIdx]) {
            continue;
        }
        auto* childCol = batch.children[arrowColIdx];
        ArrowUtils::readArrowValues(edgeListSchema.children[arrowColIdx], childCol,
            *relScanState.outputVectors[outCol], childCol->offset + rowInBatch, outputCount, 1);
    }
}

bool ArrowRelTable::scanEdgeList(const transaction::Transaction* transaction,
    TableScanState& scanState) {
    auto& relScanState = scanState.cast<RelTableScanState>();

    if (relScanState.arrowScanCompleted || !relScanState.arrowSrcKeyVector ||
        !relScanState.arrowDstKeyVector) {
        return false;
    }

    scanState.resetOutVectors();
    const bool isFwd = relScanState.direction != RelDataDirection::BWD;
    const auto outputColumnIndices = getOutputColumnIndices(scanState.columnIDs);
    uint32_t outputCount = 0;
    offset_t activeBoundOffset = INVALID_OFFSET;
    sel_t activeBoundSelPos = INVALID_SEL;
    bool hasActiveBound = false;
    constexpr uint32_t maxRows = DEFAULT_VECTOR_CAPACITY;

    while (outputCount < maxRows && relScanState.arrowCurrentBatchIdx < edgeListArrays.size()) {
        const auto& batch = edgeListArrays[relScanState.arrowCurrentBatchIdx];
        auto batchLength = ArrowUtils::getArrowBatchLength(batch);

        if (relScanState.arrowCurrentBatchOffset >= batchLength) {
            ++relScanState.arrowCurrentBatchIdx;
            relScanState.arrowCurrentBatchOffset = 0;
            continue;
        }

        auto rowInBatch = relScanState.arrowCurrentBatchOffset;
        offset_t srcOffset = INVALID_OFFSET;
        offset_t dstOffset = INVALID_OFFSET;

        if (!readEdgeEndpoints(transaction, batch, rowInBatch, relScanState, srcOffset,
                dstOffset)) {
            ++relScanState.arrowCurrentBatchOffset;
            continue;
        }

        auto boundOffset = isFwd ? srcOffset : dstOffset;
        auto boundIt = relScanState.arrowBoundNodeOffsetToSelPos.find(boundOffset);

        if (boundIt == relScanState.arrowBoundNodeOffsetToSelPos.end()) {
            ++relScanState.arrowCurrentBatchOffset;
            continue;
        }

        if (!hasActiveBound) {
            hasActiveBound = true;
            activeBoundOffset = boundOffset;
            activeBoundSelPos = boundIt->second;
        } else if (boundOffset != activeBoundOffset) {
            break; // Single-bound-node contract: stop, let next call handle this node.
        }

        auto nbrOffset = isFwd ? dstOffset : srcOffset;
        auto nbrTableID = isFwd ? getToNodeTableID() : getFromNodeTableID();
        auto globalEdgeIdx = edgeListBatchOffsets[relScanState.arrowCurrentBatchIdx] + rowInBatch;
        writeEdgeListRow(batch, rowInBatch, globalEdgeIdx, nbrOffset, nbrTableID, outputCount,
            outputColumnIndices, scanState);
        ++outputCount;
        ++relScanState.arrowCurrentBatchOffset;
    }

    if (outputCount == 0) {
        relScanState.arrowScanCompleted =
            relScanState.arrowCurrentBatchIdx >= edgeListArrays.size();
        relScanState.outState->getSelVectorUnsafe().setToFiltered(0);
        return false;
    }

    relScanState.setNodeIDVectorToFlat(activeBoundSelPos);
    auto& selVec = relScanState.outState->getSelVectorUnsafe();
    selVec.setToUnfiltered(outputCount);
    relScanState.arrowScanCompleted = relScanState.arrowCurrentBatchIdx >= edgeListArrays.size();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// CSR scan helpers
// ─────────────────────────────────────────────────────────────────────────────

void ArrowRelTable::initCsrScanState(RelTableScanState& relScanState) const {
    relScanState.arrowScanCompleted = false;
    auto& arrowScanState = relScanState.cast<ArrowRelTableScanState>();
    bool useCursor = (relScanState.direction == RelDataDirection::FWD) ||
                     (relScanState.direction == RelDataDirection::BWD && hasBwd);
    arrowScanState.csrCursor = useCursor ? std::make_optional(ArrowCsrCursor{}) : std::nullopt;
    relScanState.arrowCurrentBatchIdx = 0;
    relScanState.arrowCurrentBatchOffset = 0;
}

// Locate which batch contains globalEdgeIdx and the local row within it.
std::pair<size_t, size_t> ArrowRelTable::findBatch(uint64_t edgeIdx,
    const std::vector<size_t>& batchOffsets) const {
    if (batchOffsets.empty()) {
        return {0, 0};
    }

    // upper_bound gives first element > edgeIdx; decrement to get the batch that contains it.
    auto it = std::upper_bound(batchOffsets.begin(), batchOffsets.end(), edgeIdx);
    --it;
    size_t batchIdx = static_cast<size_t>(it - batchOffsets.begin());
    size_t localRow = static_cast<size_t>(edgeIdx - *it);
    return {batchIdx, localRow};
}

uint64_t ArrowRelTable::readNeighbourOffset(const ArrowSchema* childSchema,
    const ArrowArrayWrapper& batch, size_t row, ValueVector& scratchVec) const {
    if (batch.n_children < 1 || !batch.children || !batch.children[0]) {
        return INVALID_OFFSET;
    }

    const auto* col = batch.children[0];
    if (!col->buffers || !col->buffers[1]) {
        return INVALID_OFFSET;
    }

    ArrowUtils::readArrowValues(childSchema, col, scratchVec, col->offset + row, 0, 1);
    if (scratchVec.isNull(0)) {
        return INVALID_OFFSET;
    }
    return scratchVec.getValue<uint64_t>(0);
}

uint64_t ArrowRelTable::findSourceNodeForRow(uint64_t edgeIdx, const IndptrView& indptr) {
    if (indptr.empty()) {
        return 0;
    }

    // Manual upper_bound: find first index k s.t. indptr[k] > edgeIdx,
    // then step back one to get the last k where indptr[k] <= edgeIdx.
    size_t lo = 0, hi = indptr.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (indptr[mid] <= edgeIdx) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo == 0 ? 0 : static_cast<uint64_t>(lo - 1);
}

void ArrowRelTable::setupNodeEdgeRange(ArrowCsrCursor& cursor,
    const RelTableScanState& relScanState, const IndptrView& indptr) const {
    auto selIdx = relScanState.cachedBoundNodeSelVector[cursor.boundNodeIdx];
    auto nodeID = relScanState.nodeIDVector->getValue<nodeID_t>(selIdx);
    auto nodeOffset = nodeID.offset;

    if (nodeOffset >= indptr.size()) {
        // Node beyond indptr: no edges.
        cursor.edgeIdx = 0;
        cursor.edgeEnd = 0;
        return;
    }
    cursor.edgeIdx = indptr[nodeOffset];
    // Last node: end = totalRows (or next indptr entry if it exists).
    if (nodeOffset + 1 < indptr.size()) {
        cursor.edgeEnd = indptr[nodeOffset + 1];
    } else {
        cursor.edgeEnd = totalRows;
    }
}

bool ArrowRelTable::advanceCursorToNextBound(ArrowCsrCursor& cursor,
    const RelTableScanState& relScanState, const IndptrView& indptr) const {
    auto numBoundNodes = relScanState.cachedBoundNodeSelVector.getSelSize();
    while (cursor.boundNodeIdx < numBoundNodes) {
        if (cursor.edgeIdx < cursor.edgeEnd) {
            return true;
        }
        ++cursor.boundNodeIdx;
        if (cursor.boundNodeIdx >= numBoundNodes) {
            break;
        }
        setupNodeEdgeRange(cursor, relScanState, indptr);
    }
    return false;
}

void ArrowRelTable::writeCsrRow(uint64_t globalEdgeIdx, offset_t nbrOffset, table_id_t nbrTableID,
    uint32_t outputCount, const std::vector<int64_t>& outputColumnIndices,
    TableScanState& scanState) const {
    auto& relScanState = scanState.cast<RelTableScanState>();
    const bool isFwd = relScanState.direction == RelDataDirection::FWD;
    const auto& indexBatches = isFwd ? fwdIndices : bwdIndices;
    const auto& batchOffsets = isFwd ? fwdBatchOffsets : bwdBatchOffsets;
    const auto& indexSchema = isFwd ? fwdIndicesSchema : bwdIndicesSchema;

    if (!relScanState.outputVectors.empty()) {
        relScanState.outputVectors[0]->setValue<internalID_t>(outputCount,
            internalID_t{nbrOffset, nbrTableID});
    }

    auto [batchIdx, localRow] = findBatch(globalEdgeIdx, batchOffsets);
    if (batchIdx >= indexBatches.size()) {
        return;
    }
    const auto& batch = indexBatches[batchIdx];
    auto numChildren = batch.n_children < 0 ? 0u : static_cast<uint64_t>(batch.n_children);

    for (uint64_t outCol = 1; outCol < relScanState.outputVectors.size(); ++outCol) {
        if (!relScanState.outputVectors[outCol]) {
            continue;
        }
        if (outCol < scanState.columnIDs.size() &&
            scanState.columnIDs[outCol] == REL_ID_COLUMN_ID) {
            relScanState.outputVectors[outCol]->setValue<internalID_t>(outputCount,
                internalID_t{static_cast<offset_t>(globalEdgeIdx), getTableID()});
            continue;
        }
        if (outCol >= outputColumnIndices.size()) {
            continue;
        }
        auto arrowColIdx = outputColumnIndices[outCol];
        if (arrowColIdx < 0 || static_cast<uint64_t>(arrowColIdx) >= numChildren ||
            !batch.children[arrowColIdx] || !indexSchema.children[arrowColIdx]) {
            continue;
        }
        auto* childCol = batch.children[arrowColIdx];
        ArrowUtils::readArrowValues(indexSchema.children[arrowColIdx], childCol,
            *relScanState.outputVectors[outCol], childCol->offset + localRow, outputCount, 1);
    }
}

bool ArrowRelTable::scanCsrWithCursor(TableScanState& scanState,
    const std::vector<ArrowArrayWrapper>& indices, const ArrowSchemaWrapper& indicesSchema,
    const std::vector<size_t>& indexBatchOffsets, const IndptrView& indptr, table_id_t nbrTableID) {
    auto& relScanState = scanState.cast<RelTableScanState>();
    auto& arrowScanState = relScanState.cast<ArrowRelTableScanState>();
    auto& cursor = *arrowScanState.csrCursor;
    const auto numBoundNodes = relScanState.cachedBoundNodeSelVector.getSelSize();

    if (cursor.boundNodeIdx == 0 && cursor.edgeIdx == 0 && cursor.edgeEnd == 0 &&
        numBoundNodes > 0) {
        setupNodeEdgeRange(cursor, relScanState, indptr);
    }

    const auto outputColumnIndices = getOutputColumnIndices(scanState.columnIDs);
    const auto* childSchema = (indicesSchema.n_children > 0 && indicesSchema.children) ?
                                  indicesSchema.children[0] :
                                  nullptr;
    auto singleState = DataChunkState::getSingleValueDataChunkState();
    ValueVector nbrOffsetVec{LogicalType::UINT64(), memoryManager, singleState};
    nbrOffsetVec.state->setToFlat();
    constexpr uint32_t maxRows = DEFAULT_VECTOR_CAPACITY;

    // Loop over bound nodes. Only returns false when advanceCursorToNextBound exhausts all of
    // them; a zero-output iteration (data corruption) retries the next bound node instead of
    // returning early.
    while (advanceCursorToNextBound(cursor, relScanState, indptr)) {
        scanState.resetOutVectors();
        const auto selIdx = relScanState.cachedBoundNodeSelVector[cursor.boundNodeIdx];

        // findBatch once per bound-node entry, then advance batch position incrementally
        // inside the inner loop — avoids a O(log numBatches) binary search per edge.
        auto [curBatchIdx, curLocalRow] = findBatch(cursor.edgeIdx, indexBatchOffsets);
        uint32_t outputCount = 0;

        while (outputCount < maxRows && cursor.edgeIdx < cursor.edgeEnd) {
            if (curBatchIdx >= indices.size()) {
                cursor.edgeIdx = cursor.edgeEnd; // data corruption: force-exhaust this node
                break;
            }
            const auto& curBatch = indices[curBatchIdx];
            auto nbrOffset = readNeighbourOffset(childSchema, curBatch, curLocalRow, nbrOffsetVec);
            writeCsrRow(cursor.edgeIdx, nbrOffset, nbrTableID, outputCount, outputColumnIndices,
                scanState);
            ++outputCount;
            ++cursor.edgeIdx;
            if (++curLocalRow >= ArrowUtils::getArrowBatchLength(curBatch)) {
                ++curBatchIdx;
                curLocalRow = 0;
            }
        }

        if (cursor.edgeIdx >= cursor.edgeEnd) {
            ++cursor.boundNodeIdx;
            if (cursor.boundNodeIdx < numBoundNodes) {
                setupNodeEdgeRange(cursor, relScanState, indptr);
            }
        }

        if (outputCount > 0) {
            relScanState.arrowScanCompleted = cursor.boundNodeIdx >= numBoundNodes;
            relScanState.setNodeIDVectorToFlat(selIdx);
            auto& selVec = relScanState.outState->getSelVectorUnsafe();
            selVec.setToFiltered(outputCount);
            for (uint32_t i = 0; i < outputCount; ++i) {
                selVec[i] = i;
            }
            return true;
        }
        // outputCount == 0 only if the data-corruption guard fired; retry next bound node.
    }

    relScanState.arrowScanCompleted = true;
    relScanState.outState->getSelVectorUnsafe().setToFiltered(0);
    return false;
}

bool ArrowRelTable::scanCsrBackwardFallback(const transaction::Transaction*,
    TableScanState& scanState) {
    auto& relScanState = scanState.cast<RelTableScanState>();
    if (relScanState.arrowScanCompleted) {
        return false;
    }

    scanState.resetOutVectors();
    const auto outputColumnIndices = getOutputColumnIndices(scanState.columnIDs);
    const auto* childSchema = (fwdIndicesSchema.n_children > 0 && fwdIndicesSchema.children) ?
                                  fwdIndicesSchema.children[0] :
                                  nullptr;
    auto singleState = DataChunkState::getSingleValueDataChunkState();
    ValueVector nbrOffsetVec{LogicalType::UINT64(), memoryManager, singleState};
    nbrOffsetVec.state->setToFlat();

    uint32_t outputCount = 0;
    constexpr uint32_t maxRows = DEFAULT_VECTOR_CAPACITY;
    sel_t activeBoundSelPos = INVALID_SEL;
    offset_t activeDstOffset = INVALID_OFFSET;
    bool hasActive = false;

    while (outputCount < maxRows && relScanState.arrowCurrentBatchIdx < fwdIndices.size()) {
        const auto& batch = fwdIndices[relScanState.arrowCurrentBatchIdx];
        auto batchLength = ArrowUtils::getArrowBatchLength(batch);
        if (relScanState.arrowCurrentBatchOffset >= batchLength) {
            ++relScanState.arrowCurrentBatchIdx;
            relScanState.arrowCurrentBatchOffset = 0;
            continue;
        }

        auto localRow = relScanState.arrowCurrentBatchOffset;
        auto dstOffset = readNeighbourOffset(childSchema, batch, localRow, nbrOffsetVec);
        auto boundIt = relScanState.arrowBoundNodeOffsetToSelPos.find(dstOffset);
        if (boundIt == relScanState.arrowBoundNodeOffsetToSelPos.end()) {
            ++relScanState.arrowCurrentBatchOffset;
            continue;
        }

        if (!hasActive) {
            hasActive = true;
            activeDstOffset = dstOffset;
            activeBoundSelPos = boundIt->second;
        } else if (dstOffset != activeDstOffset) {
            break;
        }

        auto globalEdgeIdx = fwdBatchOffsets[relScanState.arrowCurrentBatchIdx] + localRow;
        auto srcOffset = findSourceNodeForRow(globalEdgeIdx,
            IndptrView{fwdIndptr, fwdIndptrBatchOffsets, fwdIndptrTotalEntries});

        if (!relScanState.outputVectors.empty()) {
            relScanState.outputVectors[0]->setValue<internalID_t>(outputCount,
                internalID_t{srcOffset, getFromNodeTableID()});
        }
        const auto& indexSchema = fwdIndicesSchema;
        auto numChildren = batch.n_children < 0 ? 0u : static_cast<uint64_t>(batch.n_children);
        for (uint64_t outCol = 1; outCol < relScanState.outputVectors.size(); ++outCol) {
            if (!relScanState.outputVectors[outCol]) {
                continue;
            }
            if (outCol < scanState.columnIDs.size() &&
                scanState.columnIDs[outCol] == REL_ID_COLUMN_ID) {
                relScanState.outputVectors[outCol]->setValue<internalID_t>(outputCount,
                    internalID_t{static_cast<offset_t>(globalEdgeIdx), getTableID()});
                continue;
            }
            if (outCol >= outputColumnIndices.size()) {
                continue;
            }
            auto arrowColIdx = outputColumnIndices[outCol];
            if (arrowColIdx < 0 || static_cast<uint64_t>(arrowColIdx) >= numChildren ||
                !batch.children[arrowColIdx] || !indexSchema.children[arrowColIdx]) {
                continue;
            }
            auto* childCol = batch.children[arrowColIdx];
            ArrowUtils::readArrowValues(indexSchema.children[arrowColIdx], childCol,
                *relScanState.outputVectors[outCol], childCol->offset + localRow, outputCount, 1);
        }

        ++outputCount;
        ++relScanState.arrowCurrentBatchOffset;
    }

    if (outputCount == 0) {
        relScanState.arrowScanCompleted = relScanState.arrowCurrentBatchIdx >= fwdIndices.size();
        relScanState.outState->getSelVectorUnsafe().setToFiltered(0);
        return false;
    }

    relScanState.setNodeIDVectorToFlat(activeBoundSelPos);
    auto& selVec = relScanState.outState->getSelVectorUnsafe();
    selVec.setToUnfiltered(outputCount);
    relScanState.arrowScanCompleted = relScanState.arrowCurrentBatchIdx >= fwdIndices.size();
    return true;
}

bool ArrowRelTable::scanCsr(const transaction::Transaction* transaction,
    TableScanState& scanState) {
    auto& relScanState = scanState.cast<RelTableScanState>();
    if (relScanState.direction == RelDataDirection::FWD) {
        const auto indptr = IndptrView{fwdIndptr, fwdIndptrBatchOffsets, fwdIndptrTotalEntries};
        return scanCsrWithCursor(scanState, fwdIndices, fwdIndicesSchema, fwdBatchOffsets, indptr,
            getToNodeTableID());
    }
    if (hasBwd) {
        const auto indptr = IndptrView{bwdIndptr, bwdIndptrBatchOffsets, bwdIndptrTotalEntries};
        return scanCsrWithCursor(scanState, bwdIndices, bwdIndicesSchema, bwdBatchOffsets, indptr,
            getFromNodeTableID());
    }
    return scanCsrBackwardFallback(transaction, scanState);
}

bool ArrowRelTable::scanInternal(transaction::Transaction* transaction, TableScanState& scanState) {
    if (layout == ArrowRelLayout::EdgeList) {
        return scanEdgeList(transaction, scanState);
    }
    return scanCsr(transaction, scanState);
}

std::vector<int64_t> ArrowRelTable::getOutputColumnIndices(
    const std::vector<column_id_t>& columnIDs) const {
    std::vector<int64_t> result(columnIDs.size(), -1);
    for (size_t i = 0; i < columnIDs.size(); ++i) {
        auto columnID = columnIDs[i];
        if (columnID == NBR_ID_COLUMN_ID || columnID == INVALID_COLUMN_ID ||
            columnID == ROW_IDX_COLUMN_ID) {
            continue;
        }
        if (propertyColumnToArrowColumnIdx.contains(columnID)) {
            result[i] = propertyColumnToArrowColumnIdx.at(columnID);
        }
    }
    return result;
}

row_idx_t ArrowRelTable::getTotalRowCount(
    [[maybe_unused]] const transaction::Transaction* transaction) const {
    return totalRows;
}

} // namespace storage
} // namespace lbug

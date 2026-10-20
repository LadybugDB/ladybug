#include "storage/table/rel_table.h"

#include <algorithm>
#include <map>
#include <queue>
#include <type_traits>

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/message.h"
#include "common/exception/runtime.h"
#include "common/types/types.h"
#include "main/client_context.h"
#include "storage/local_storage/local_rel_table.h"
#include "storage/local_storage/local_storage.h"
#include "storage/local_storage/local_table.h"
#include "storage/storage_manager.h"
#include "storage/storage_utils.h"
#include "storage/table/column_chunk.h"
#include "storage/table/column_chunk_data.h"
#include "storage/table/rel_table_data.h"
#include "storage/wal/local_wal.h"
#include "transaction/transaction.h"
#include <format>
#include <ranges>

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::transaction;
using namespace lbug::evaluator;

namespace lbug {
namespace storage {

RelTableScanState::RelTableScanState(MemoryManager& mm, common::ValueVector* nodeIDVector,
    std::vector<common::ValueVector*> outputVectors,
    std::shared_ptr<common::DataChunkState> outChunkState, bool randomLookup)
    : TableScanState{nodeIDVector, std::move(outputVectors), std::move(outChunkState)},
      direction{common::RelDataDirection::INVALID}, currBoundNodeIdx{0}, csrOffsetColumn{nullptr},
      csrLengthColumn{nullptr}, randomLookup{randomLookup}, localTableScanState{nullptr} {
    nodeGroupScanState = std::make_unique<CSRNodeGroupScanState>(mm, randomLookup);
}

RelTableScanState::RelTableScanState(common::ValueVector* nodeIDVector,
    std::vector<common::ValueVector*> outputVectors,
    std::shared_ptr<common::DataChunkState> outChunkState)
    : TableScanState{nodeIDVector, std::move(outputVectors), std::move(outChunkState)},
      direction{common::RelDataDirection::INVALID}, currBoundNodeIdx{0}, csrOffsetColumn{nullptr},
      csrLengthColumn{nullptr}, randomLookup{false}, localTableScanState{nullptr} {
    nodeGroupScanState = std::make_unique<CSRNodeGroupScanState>();
}

void RelTableScanState::setToTable(const Transaction* transaction, Table* table_,
    std::vector<column_id_t> columnIDs_, std::vector<ColumnPredicateSet> columnPredicateSets_,
    RelDataDirection direction_) {
    TableScanState::setToTable(transaction, table_, std::move(columnIDs_),
        std::move(columnPredicateSets_));
    localTableScanState.reset();
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
    if (const auto localRelTable = transaction->getLocalStorage()->getLocalTable(*table)) {
        auto localTableColumnIDs = LocalRelTable::rewriteLocalColumnIDs(direction, columnIDs);
        localTableScanState = std::make_unique<LocalRelTableScanState>(*this,
            localRelTable->ptrCast<LocalRelTable>(), localTableColumnIDs);
    }
}

void RelTableScanState::initState(Transaction* transaction, NodeGroup* nodeGroup,
    bool resetCachedBoundNodeIDs) {
    this->nodeGroup = nodeGroup;
    if (resetCachedBoundNodeIDs) {
        initCachedBoundNodeIDSelVector();
    }
    if (this->nodeGroup) {
        initStateForCommitted(transaction);
    } else if (hasUnCommittedData()) {
        initStateForUncommitted();
    } else {
        source = TableScanSource::NONE;
    }
}

void RelTableScanState::initCachedBoundNodeIDSelVector() {
    if (nodeIDVector->state->getSelVector().isUnfiltered()) {
        cachedBoundNodeSelVector.setToUnfiltered();
    } else {
        cachedBoundNodeSelVector.setToFiltered();
        memcpy(cachedBoundNodeSelVector.getMutableBuffer().data(),
            nodeIDVector->state->getSelVectorUnsafe().getMutableBuffer().data(),
            nodeIDVector->state->getSelVector().getSelSize() * sizeof(sel_t));
    }
    cachedBoundNodeSelVector.setSelSize(nodeIDVector->state->getSelVector().getSelSize());
}

void RelTableScanState::releaseScanPin() {
    if (auto* csrScanState = dynamic_cast<CSRNodeGroupScanState*>(nodeGroupScanState.get())) {
        csrScanState->pin.release();
    }
}

bool RelTableScanState::hasUnCommittedData() const {
    return localTableScanState && localTableScanState->localRelTable;
}

void RelTableScanState::initStateForCommitted(const Transaction* transaction) {
    source = TableScanSource::COMMITTED;
    currBoundNodeIdx = 0;
    nodeGroup->initializeScanState(transaction, *this);
}

void RelTableScanState::initStateForUncommitted() {
    DASSERT(localTableScanState);
    source = TableScanSource::UNCOMMITTED;
    currBoundNodeIdx = 0;
    localTableScanState->localRelTable->initializeScan(*this);
}

void RelTableScanState::setCachedBoundNodeSelVectorToPositions(
    const std::vector<common::sel_t>& positions) {
    DASSERT(!positions.empty());
    cachedBoundNodeSelVector.setToFiltered();
    std::memcpy(cachedBoundNodeSelVector.getMutableBuffer().data(), positions.data(),
        positions.size() * sizeof(common::sel_t));
    cachedBoundNodeSelVector.setSelSize(positions.size());
}

bool RelTableScanState::startNextBoundNodeGroup(Transaction* transaction) {
    if (boundNodeGroupPlan.empty()) {
        return false;
    }
    auto [nodeGroupToScan, positions] = std::move(boundNodeGroupPlan.front());
    boundNodeGroupPlan.erase(boundNodeGroupPlan.begin());
    // Note: deliberately do NOT update nodeGroupIdx here. CSRNodeGroup::initializeScanState
    // detects the node group change (stale nodeGroupIdx != its own idx) and runs
    // initScanForCommittedPersistent, which loads this group's CSR header.
    setCachedBoundNodeSelVectorToPositions(positions);
    // initState sets this->nodeGroup, resets currBoundNodeIdx and initializes the CSR scan
    // state of the node group. reset=false: cachedBoundNodeSelVector already holds exactly
    // the positions of this group's bound nodes.
    initState(transaction, nodeGroupToScan, false /*resetCachedBoundNodeIDs*/);
    return true;
}

bool RelTableScanState::scanNext(Transaction* transaction) {
    while (true) {
        switch (source) {
        case TableScanSource::COMMITTED: {
            const auto scanResult = nodeGroup->scan(transaction, *this);
            if (scanResult == NODE_GROUP_SCAN_EMPTY_RESULT) {
                if (startNextBoundNodeGroup(transaction)) {
                    continue;
                }
                if (!allBoundNodePositions.empty()) {
                    // Restore the full set of bound node positions so that the local
                    // (uncommitted) scan and downstream resume paths iterate over the
                    // whole batch, not just the last node group's subset.
                    setCachedBoundNodeSelVectorToPositions(allBoundNodePositions);
                }
                if (hasUnCommittedData()) {
                    initStateForUncommitted();
                } else {
                    source = TableScanSource::NONE;
                }
                continue;
            }
            return true;
        }
        case TableScanSource::UNCOMMITTED: {
            DASSERT(localTableScanState && localTableScanState->localRelTable);
            return localTableScanState->localRelTable->scan(transaction, *this);
        }
        case TableScanSource::NONE: {
            return false;
        }
        default: {
            UNREACHABLE_CODE;
        }
        }
    }
}

void RelTableScanState::setNodeIDVectorToFlat(sel_t selPos) const {
    nodeIDVector->state->setToFlat();
    nodeIDVector->state->getSelVectorUnsafe().setToFiltered(1);
    nodeIDVector->state->getSelVectorUnsafe()[0] = selPos;
}

RelTable::RelTable(RelGroupCatalogEntry* relGroupEntry, table_id_t fromTableID,
    table_id_t toTableID, const StorageManager* storageManager, MemoryManager* memoryManager)
    : Table{relGroupEntry, storageManager, memoryManager}, fromNodeTableID{fromTableID},
      toNodeTableID{toTableID}, nextRelOffset{0} {
    auto relEntryInfo = relGroupEntry->getRelEntryInfo(fromNodeTableID, toNodeTableID);
    tableID = relEntryInfo->oid;
    relGroupID = relGroupEntry->getTableID();
    // Foreign-backed rel tables (e.g. pg_client rel_ tables) are scan-driven and don't
    // own on-disk CSR columns, so skip RelTableData construction for them.
    if (!relGroupEntry->getScanFunction().has_value()) {
        for (auto direction : relGroupEntry->getRelDataDirections()) {
            auto nbrTableID = RelDirectionUtils::getNbrTableID(direction, fromTableID, toTableID);
            directedRelData.emplace_back(std::make_unique<RelTableData>(storageManager->getDataFH(),
                memoryManager, shadowFile, *relGroupEntry, *relEntryInfo, *this, direction,
                nbrTableID, enableCompression));
        }
    }
}

void RelTable::initScanState(Transaction* transaction, TableScanState& scanState,
    bool resetCachedBoundNodeSelVec) const {
    auto& relScanState = scanState.cast<RelTableScanState>();
    // Collect the positions of this batch's bound nodes. On a fresh batch they come from
    // the input sel vector; when resuming a batch (e.g. a multi rel table scan moving to
    // the next rel table over the same bound nodes) they were cached by the previous init.
    // Reusable workspace (no per-batch allocation): the batch's bound-node positions.
    auto& positions = relScanState.positionsBuffer;
    const auto& sel = resetCachedBoundNodeSelVec ?
                          relScanState.nodeIDVector->state->getSelVector() :
                          relScanState.cachedBoundNodeSelVector;
    positions.resize(sel.getSelSize());
    if (positions.empty()) {
        // No bound nodes in this batch: drop any stale multi-group plan from the previous
        // batch so scanNext() observes an exhausted batch instead of rescanning it.
        relScanState.boundNodeGroupPlan.clear();
        relScanState.allBoundNodePositions.clear();
        scanState.source = TableScanSource::NONE;
        return;
    }
    bool alreadySorted = true;
    common::offset_t prevOffset = 0;
    for (auto i = 0u; i < sel.getSelSize(); i++) {
        positions[i] = sel[i];
        const auto offset = relScanState.nodeIDVector->readNodeOffset(sel[i]);
        if (i > 0 && offset < prevOffset) {
            alreadySorted = false;
        }
        prevOffset = offset;
    }
    // The CSR scan processes the bound nodes of a batch in a single forward pass over the
    // CSR lists, which are laid out in ascending bound-node-offset order within each node
    // group. Bound node IDs are only guaranteed to be ascending when the input is a node
    // table scan. When we extend from the output of a previous extend (e.g. planning
    // (a)->(b)->(c) as scan(a) -> extend(a->b) -> extend(b->c)), the bound nodes are the
    // children of a single parent and come in arbitrary order, so sort them by offset.
    // Parents whose CSR list lies before the current scan position would otherwise be
    // silently skipped by CSRNodeGroup::scanCommittedPersistentWithCache. Batches that
    // already arrive in ascending offset order skip the sort.
    if (!alreadySorted) {
        std::sort(positions.begin(), positions.end(), [&](common::sel_t a, common::sel_t b) {
            return relScanState.nodeIDVector->readNodeOffset(a) <
                   relScanState.nodeIDVector->readNodeOffset(b);
        });
    }
    // Partition the bound nodes by node group. Offsets are ascending now, so node groups
    // form contiguous runs: comparing the first/last group detects the common
    // single-group case without a tree or per-group vectors. The children of a single
    // parent can still span multiple node groups, in which case the batch is scanned one
    // node group at a time; see RelTableScanState::boundNodeGroupPlan.
    const auto firstGroupIdx =
        StorageUtils::getNodeGroupIdx(relScanState.nodeIDVector->readNodeOffset(positions.front()));
    const auto lastGroupIdx =
        StorageUtils::getNodeGroupIdx(relScanState.nodeIDVector->readNodeOffset(positions.back()));
    if (firstGroupIdx == lastGroupIdx) {
        // Single node group (the common case): historical fast path.
        relScanState.boundNodeGroupPlan.clear();
        relScanState.allBoundNodePositions.clear();
        // Note there we directly read node at pos 0 here regardless the selVector is
        // filtered or not. This is because we're assuming the nodeIDVector is always a
        // sequence here.
        const auto boundNodePos = resetCachedBoundNodeSelVec ?
                                      relScanState.nodeIDVector->state->getSelVector()[0] :
                                      relScanState.cachedBoundNodeSelVector[0];
        const auto boundNodeID = relScanState.nodeIDVector->getValue<nodeID_t>(boundNodePos);
        NodeGroup* nodeGroup = nullptr;
        // Check if the node group idx is same as previous scan.
        const auto nodeGroupIdx = StorageUtils::getNodeGroupIdx(boundNodeID.offset);
        if (relScanState.nodeGroupIdx != nodeGroupIdx) {
            // We need to re-initialize the node group scan state.
            nodeGroup = getDirectedTableData(relScanState.direction)->getNodeGroup(nodeGroupIdx);
        } else {
            nodeGroup = relScanState.nodeGroup;
        }
        // If the sorted bound node order differs from the input sel vector order, install
        // the sorted positions into cachedBoundNodeSelVector (initState would otherwise
        // re-copy the unsorted input order and out-of-order parents would be skipped by
        // the forward CSR scan).
        auto inputSelMatchesSorted = true;
        if (resetCachedBoundNodeSelVec) {
            const auto& inputSel = relScanState.nodeIDVector->state->getSelVector();
            for (auto i = 0u; i < positions.size(); i++) {
                if (positions[i] != inputSel[i]) {
                    inputSelMatchesSorted = false;
                    break;
                }
            }
        }
        if (resetCachedBoundNodeSelVec && !inputSelMatchesSorted) {
            relScanState.setCachedBoundNodeSelVectorToPositions(positions);
            scanState.initState(transaction, nodeGroup, false /*resetCachedBoundNodeIDs*/);
        } else {
            scanState.initState(transaction, nodeGroup, resetCachedBoundNodeSelVec);
        }
        return;
    }
    // Multi-group batch: scan one node group at a time. Offsets are ascending, so node
    // groups form contiguous runs over the sorted positions: linear run partition, no map.
    // allBoundNodePositions keeps a copy (reusing its capacity across batches); the
    // workspace buffer stays intact for the next batch.
    relScanState.allBoundNodePositions = positions;
    relScanState.boundNodeGroupPlan.clear();
    auto runStart = 0u;
    auto runGroupIdx = firstGroupIdx;
    for (auto k = 1u;; ++k) {
        const auto runEnds = (k == positions.size());
        const auto groupIdx = runEnds ?
                                  runGroupIdx :
                                  StorageUtils::getNodeGroupIdx(
                                      relScanState.nodeIDVector->readNodeOffset(positions[k]));
        if (runEnds || groupIdx != runGroupIdx) {
            std::vector<common::sel_t> runPositions(positions.begin() + runStart,
                positions.begin() + k);
            relScanState.boundNodeGroupPlan.emplace_back(
                getDirectedTableData(relScanState.direction)->getNodeGroup(runGroupIdx),
                std::move(runPositions));
            if (runEnds) {
                break;
            }
            runStart = k;
            runGroupIdx = groupIdx;
        }
    }
    relScanState.startNextBoundNodeGroup(transaction);
}

bool RelTable::scanInternal(Transaction* transaction, TableScanState& scanState) {
    return scanState.scanNext(transaction);
}

static void throwRelMultiplicityConstraintError(const std::string& tableName, offset_t nodeOffset,
    RelDataDirection direction) {
    throw RuntimeException(ExceptionMessage::violateRelMultiplicityConstraint(tableName,
        std::to_string(nodeOffset), RelDirectionUtils::relDirectionToString(direction)));
}

void RelTable::checkRelMultiplicityConstraint(Transaction* transaction,
    const TableInsertState& state) const {
    const auto& insertState = state.constCast<RelTableInsertState>();
    DASSERT(insertState.srcNodeIDVector.state->getSelVector().getSelSize() == 1 &&
            insertState.dstNodeIDVector.state->getSelVector().getSelSize() == 1);

    for (auto& relData : directedRelData) {
        if (relData->getMultiplicity() == RelMultiplicity::ONE) {
            throwIfNodeHasRels(transaction, relData->getDirection(),
                &insertState.getBoundNodeIDVector(relData->getDirection()),
                throwRelMultiplicityConstraintError);
        }
    }
}

void RelTable::insert(Transaction* transaction, TableInsertState& insertState) {
    checkRelMultiplicityConstraint(transaction, insertState);

    DASSERT(transaction->getLocalStorage());
    const auto localTable = transaction->getLocalStorage()->getOrCreateLocalTable(*this);
    localTable->insert(transaction, insertState);
    if (insertState.logToWAL && transaction->shouldLogToWAL()) {
        DASSERT(transaction->isWriteTransaction());
        const auto& relInsertState = insertState.cast<RelTableInsertState>();
        std::vector<ValueVector*> vectorsToLog;
        vectorsToLog.push_back(&relInsertState.srcNodeIDVector);
        vectorsToLog.push_back(&relInsertState.dstNodeIDVector);
        vectorsToLog.insert(vectorsToLog.end(), relInsertState.propertyVectors.begin(),
            relInsertState.propertyVectors.end());
        DASSERT(relInsertState.srcNodeIDVector.state->getSelVector().getSelSize() == 1);
        auto& wal = transaction->getLocalWAL();
        wal.logTableInsertion(getOwnerCatalogName(), tableID, TableType::REL,
            relInsertState.srcNodeIDVector.state->getSelVector().getSelSize(), vectorsToLog);
    }
    setHasChanges();
}

void RelTable::update(Transaction* transaction, TableUpdateState& updateState) {
    const auto& relUpdateState = updateState.cast<RelTableUpdateState>();
    DASSERT(relUpdateState.relIDVector.state->getSelVector().getSelSize() == 1);
    const auto relIDPos = relUpdateState.relIDVector.state->getSelVector()[0];
    if (const auto relOffset = relUpdateState.relIDVector.readNodeOffset(relIDPos);
        relOffset >= StorageConstants::MAX_NUM_ROWS_IN_TABLE) {
        const auto localTable = transaction->getLocalStorage()->getLocalTable(*this);
        DASSERT(localTable);
        localTable->update(&DUMMY_TRANSACTION, updateState);
    } else {
        for (auto& relData : directedRelData) {
            relData->update(transaction,
                relUpdateState.getBoundNodeIDVector(relData->getDirection()),
                relUpdateState.relIDVector, relUpdateState.columnID, relUpdateState.propertyVector);
        }
    }
    if (updateState.logToWAL && transaction->shouldLogToWAL()) {
        DASSERT(transaction->isWriteTransaction());
        auto& wal = transaction->getLocalWAL();
        wal.logRelUpdate(getOwnerCatalogName(), tableID, relUpdateState.columnID,
            &relUpdateState.srcNodeIDVector, &relUpdateState.dstNodeIDVector,
            &relUpdateState.relIDVector, &relUpdateState.propertyVector);
    }
    setHasChanges();
}

bool RelTable::delete_(Transaction* transaction, TableDeleteState& deleteState) {
    const auto& relDeleteState = deleteState.cast<RelTableDeleteState>();
    DASSERT(relDeleteState.relIDVector.state->getSelVector().getSelSize() == 1);
    const auto relIDPos = relDeleteState.relIDVector.state->getSelVector()[0];
    bool isDeleted = false;
    if (const auto relOffset = relDeleteState.relIDVector.readNodeOffset(relIDPos);
        relOffset >= StorageConstants::MAX_NUM_ROWS_IN_TABLE) {
        const auto localTable = transaction->getLocalStorage()->getLocalTable(*this);
        DASSERT(localTable);
        isDeleted = localTable->delete_(transaction, deleteState);
    } else {
        for (auto& relData : directedRelData) {
            isDeleted = relData->delete_(transaction,
                relDeleteState.getBoundNodeIDVector(relData->getDirection()),
                relDeleteState.relIDVector);
            if (!isDeleted) {
                break;
            }
        }
    }
    if (isDeleted) {
        setHasChanges();
        if (deleteState.logToWAL && transaction->shouldLogToWAL()) {
            DASSERT(transaction->isWriteTransaction());
            auto& wal = transaction->getLocalWAL();
            wal.logRelDelete(getOwnerCatalogName(), tableID, &relDeleteState.srcNodeIDVector,
                &relDeleteState.dstNodeIDVector, &relDeleteState.relIDVector);
        }
    }
    return isDeleted;
}

void RelTable::detachDelete(Transaction* transaction, RelTableDeleteState* deleteState) {
    auto direction = deleteState->detachDeleteDirection;
    if (std::ranges::count(getStorageDirections(), direction) == 0) {
        throw RuntimeException(
            std::format("Cannot delete edges of direction {} from table {} as they do not exist.",
                RelDirectionUtils::relDirectionToString(direction), tableName));
    }
    DASSERT(deleteState->srcNodeIDVector.state->getSelVector().getSelSize() == 1);
    const auto tableData = getDirectedTableData(direction);
    const auto reverseTableData =
        directedRelData.size() == NUM_REL_DIRECTIONS ?
            getDirectedTableData(RelDirectionUtils::getOppositeDirection(direction)) :
            nullptr;
    auto relReadState =
        std::make_unique<RelTableScanState>(*memoryManager, &deleteState->srcNodeIDVector,
            std::vector{&deleteState->dstNodeIDVector, &deleteState->relIDVector},
            deleteState->dstNodeIDVector.state, true /*randomLookup*/);
    relReadState->setToTable(transaction, this, {NBR_ID_COLUMN_ID, REL_ID_COLUMN_ID}, {},
        direction);
    initScanState(transaction, *relReadState);
    detachDeleteForCSRRels(transaction, tableData, reverseTableData, relReadState.get(),
        deleteState);
    if (deleteState->logToWAL && transaction->shouldLogToWAL()) {
        DASSERT(transaction->isWriteTransaction());
        auto& wal = transaction->getLocalWAL();
        wal.logRelDetachDelete(getOwnerCatalogName(), tableID, direction,
            &deleteState->srcNodeIDVector);
    }
    setHasChanges();
}

void RelTable::detachDeleteBatch(Transaction* transaction, ValueVector& srcNodeIDVector,
    ValueVector& dstNodeIDVector, ValueVector& relIDVector, RelDataDirection direction) {
    if (std::ranges::count(getStorageDirections(), direction) == 0) {
        return;
    }
    const auto& srcSelVector = srcNodeIDVector.state->getSelVector();
    const auto numNodes = srcSelVector.getSelSize();
    if (numNodes == 0) {
        return;
    }

    std::vector<nodeID_t> srcNodeIDs;
    srcNodeIDs.reserve(numNodes);
    for (auto i = 0u; i < numNodes; i++) {
        const auto pos = srcSelVector[i];
        if (!srcNodeIDVector.isNull(pos)) {
            srcNodeIDs.push_back(srcNodeIDVector.getValue<nodeID_t>(pos));
        }
    }
    if (srcNodeIDs.empty()) {
        return;
    }

    const auto tableData = getDirectedTableData(direction);
    const auto reverseTableData =
        directedRelData.size() == NUM_REL_DIRECTIONS ?
            getDirectedTableData(RelDirectionUtils::getOppositeDirection(direction)) :
            nullptr;
    const auto localTable = transaction->getLocalStorage()->getLocalTable(*this);

    for (const auto srcNodeID : srcNodeIDs) {
        const auto srcState = std::make_shared<DataChunkState>();
        ValueVector tempSrcNodeID(LogicalType::INTERNAL_ID());
        tempSrcNodeID.setState(srcState);
        srcState->getSelVectorUnsafe().setSelSize(1);
        tempSrcNodeID.setValue(0, srcNodeID);

        auto relReadState = std::make_unique<RelTableScanState>(*memoryManager, &tempSrcNodeID,
            std::vector{&dstNodeIDVector, &relIDVector}, dstNodeIDVector.state,
            true /*randomLookup*/);
        relReadState->setToTable(transaction, this, {NBR_ID_COLUMN_ID, REL_ID_COLUMN_ID}, {},
            direction);
        initScanState(transaction, *relReadState);

        const auto relState = dstNodeIDVector.state.get();
        while (scan(transaction, *relReadState)) {
            const auto numRelsScanned = relState->getSelVector().getSelSize();
            if (relState->getSelVector().isUnfiltered()) {
                relState->getSelVectorUnsafe().setRange(0, numRelsScanned);
            }
            relState->getSelVectorUnsafe().setToFiltered(1);

            for (auto i = 0u; i < numRelsScanned; i++) {
                relState->getSelVectorUnsafe()[0] = relIDVector.state->getSelVector()[i];

                const auto relIDPos = relIDVector.state->getSelVector()[0];
                const auto relOffset = relIDVector.readNodeOffset(relIDPos);
                if (relOffset >= StorageConstants::MAX_NUM_ROWS_IN_TABLE) {
                    DASSERT(localTable);
                    RelTableDeleteState localDeleteState{tempSrcNodeID, dstNodeIDVector,
                        relIDVector, direction};
                    localTable->delete_(transaction, localDeleteState);
                    continue;
                }
                [[maybe_unused]] const auto deleted =
                    tableData->delete_(transaction, tempSrcNodeID, relIDVector);
                if (reverseTableData) {
                    [[maybe_unused]] const auto reverseDeleted =
                        reverseTableData->delete_(transaction, dstNodeIDVector, relIDVector);
                    DASSERT(deleted == reverseDeleted);
                }
            }
            relState->getSelVectorUnsafe().setToUnfiltered();
        }
    }
    if (transaction->shouldLogToWAL()) {
        DASSERT(transaction->isWriteTransaction());
        transaction->getLocalWAL().logRelDetachDelete(getOwnerCatalogName(), tableID, direction,
            &srcNodeIDVector);
    }
    setHasChanges();
}

std::vector<RelDataDirection> RelTable::getStorageDirections() const {
    std::vector<RelDataDirection> ret;
    for (const auto& relData : directedRelData) {
        ret.push_back(relData->getDirection());
    }
    return ret;
}

bool RelTable::checkIfNodeHasRels(Transaction* transaction, RelDataDirection direction,
    ValueVector* srcNodeIDVector) const {
    bool hasRels = false;
    const auto localTable = transaction->getLocalStorage()->getLocalTable(*this);
    if (localTable) {
        hasRels = localTable->cast<LocalRelTable>().checkIfNodeHasRels(srcNodeIDVector, direction);
    }
    hasRels = hasRels ||
              getDirectedTableData(direction)->checkIfNodeHasRels(transaction, srcNodeIDVector);
    return hasRels;
}

void RelTable::throwIfNodeHasRels(Transaction* transaction, RelDataDirection direction,
    ValueVector* srcNodeIDVector, const rel_multiplicity_constraint_throw_func_t& throwFunc) const {
    const auto nodeIDPos = srcNodeIDVector->state->getSelVector()[0];
    const auto nodeOffset = srcNodeIDVector->getValue<nodeID_t>(nodeIDPos).offset;
    if (checkIfNodeHasRels(transaction, direction, srcNodeIDVector)) {
        throwFunc(tableName, nodeOffset, direction);
    }
}

void RelTable::detachDeleteForCSRRels(Transaction* transaction, RelTableData* tableData,
    RelTableData* reverseTableData, RelTableScanState* relDataReadState,
    RelTableDeleteState* deleteState) {
    const auto localTable = transaction->getLocalStorage()->getLocalTable(*this);
    const auto tempState = deleteState->dstNodeIDVector.state.get();
    while (scan(transaction, *relDataReadState)) {
        const auto numRelsScanned = tempState->getSelVector().getSelSize();

        // rel table data delete_() expects the input to be flat
        // so we manually flatten the scanned rels here
        // also if the scanned state is unfiltered we need to copy over the unfiltered values to the
        // filtered buffer
        // TODO(Royi/Guodong) remove this once delete_() supports unflat vectors
        if (tempState->getSelVector().isUnfiltered()) {
            tempState->getSelVectorUnsafe().setRange(0, numRelsScanned);
        }
        tempState->getSelVectorUnsafe().setToFiltered(1);

        for (auto i = 0u; i < numRelsScanned; i++) {
            tempState->getSelVectorUnsafe()[0] = deleteState->relIDVector.state->getSelVector()[i];

            const auto relIDPos = deleteState->relIDVector.state->getSelVector()[0];
            const auto relOffset = deleteState->relIDVector.readNodeOffset(relIDPos);
            if (relOffset >= StorageConstants::MAX_NUM_ROWS_IN_TABLE) {
                DASSERT(localTable);
                localTable->delete_(transaction, *deleteState);
                continue;
            }
            [[maybe_unused]] const auto deleted = tableData->delete_(transaction,
                deleteState->srcNodeIDVector, deleteState->relIDVector);
            if (reverseTableData) {
                [[maybe_unused]] const auto reverseDeleted = reverseTableData->delete_(transaction,
                    deleteState->dstNodeIDVector, deleteState->relIDVector);
                DASSERT(deleted == reverseDeleted);
            }
        }
        tempState->getSelVectorUnsafe().setToUnfiltered();
    }
}

void RelTable::addColumn(Transaction* transaction, TableAddColumnState& addColumnState,
    PageAllocator& pageAllocator) {
    LocalTable* localTable = nullptr;
    if (transaction->getLocalStorage()) {
        localTable = transaction->getLocalStorage()->getLocalTable(*this);
    }
    if (localTable) {
        localTable->addColumn(addColumnState);
    }
    for (auto& directedRelData : directedRelData) {
        directedRelData->addColumn(addColumnState, pageAllocator);
    }
    setHasChanges();
}

RelTableData* RelTable::getDirectedTableData(RelDataDirection direction) const {
    const auto directionIdx = RelDirectionUtils::relDirectionToKeyIdx(direction);
    if (directionIdx >= directedRelData.size()) {
        throw RuntimeException(std::format(
            "Failed to get {} data for rel table \"{}\", please set the storage direction to BOTH",
            RelDirectionUtils::relDirectionToString(direction), tableName));
    }
    DASSERT(directedRelData[directionIdx]->getDirection() == direction);
    return directedRelData[directionIdx].get();
}

NodeGroup* RelTable::getOrCreateNodeGroup(const Transaction* transaction,
    node_group_idx_t nodeGroupIdx, RelDataDirection direction) const {
    return getDirectedTableData(direction)->getOrCreateNodeGroup(transaction, nodeGroupIdx);
}

void RelTable::pushInsertInfo(const Transaction* transaction, RelDataDirection direction,
    const CSRNodeGroup& nodeGroup, row_idx_t numRows_, CSRNodeGroupScanSource source) const {
    getDirectedTableData(direction)->pushInsertInfo(transaction, nodeGroup, numRows_, source);
}

void RelTable::commit(main::ClientContext* context, TableCatalogEntry* tableEntry,
    LocalTable* localTable) {
    auto& localRelTable = localTable->cast<LocalRelTable>();
    if (localRelTable.isEmpty()) {
        localTable->clear(*MemoryManager::Get(*context));
        return;
    }
    // Update relID in local storage.
    updateRelOffsets(localRelTable);
    // For both forward and backward directions, re-org local storage into compact CSR node groups.
    auto& localNodeGroup = localRelTable.getLocalNodeGroup();
    // Scan from local node group and write to WAL.
    std::vector<column_id_t> columnIDsToScan;
    for (auto i = 0u; i < localRelTable.getNumColumns(); i++) {
        columnIDsToScan.push_back(i);
    }

    std::vector<column_id_t> columnIDsToCommit;
    columnIDsToCommit.push_back(0); // NBR column.
    for (auto& property : tableEntry->getProperties()) {
        auto columnID = tableEntry->getColumnID(property.getName());
        columnIDsToCommit.push_back(columnID);
    }
    // commit rel table data
    auto transaction = transaction::Transaction::Get(*context);
    for (auto& relData : directedRelData) {
        const auto direction = relData->getDirection();
        const auto columnToSkip = (direction == RelDataDirection::FWD) ?
                                      LOCAL_BOUND_NODE_ID_COLUMN_ID :
                                      LOCAL_NBR_NODE_ID_COLUMN_ID;
        for (auto& [boundNodeOffset, rowIndices] : localRelTable.getCSRIndex(direction)) {
            auto [nodeGroupIdx, boundOffsetInGroup] =
                StorageUtils::getQuotientRemainder(boundNodeOffset, StorageConfig::NODE_GROUP_SIZE);
            auto& nodeGroup =
                relData->getOrCreateNodeGroup(transaction, nodeGroupIdx)->cast<CSRNodeGroup>();
            pushInsertInfo(transaction, direction, nodeGroup, rowIndices.size(),
                CSRNodeGroupScanSource::COMMITTED_IN_MEMORY);
            prepareCommitForNodeGroup(transaction, columnIDsToCommit, localNodeGroup, nodeGroup,
                boundOffsetInGroup, rowIndices, columnToSkip);
        }
    }

    localRelTable.clear(*MemoryManager::Get(*context));
}

void RelTable::reclaimStorage(PageAllocator& pageAllocator) const {
    for (auto& relData : directedRelData) {
        relData->reclaimStorage(pageAllocator);
    }
}

void RelTable::updateRelOffsets(const LocalRelTable& localRelTable) {
    auto& localNodeGroup = localRelTable.getLocalNodeGroup();
    const offset_t maxCommittedOffset = reserveRelOffsets(localNodeGroup.getNumRows());
    RUNTIME_CHECK(uint64_t totalNumRows = 0);
    for (auto i = 0u; i < localNodeGroup.getNumChunkedGroups(); i++) {
        const auto chunkedGroup = localNodeGroup.getChunkedNodeGroup(i);
        DASSERT(chunkedGroup);
        auto& internalIDChunk = chunkedGroup->getColumnChunk(LOCAL_REL_ID_COLUMN_ID);
        RUNTIME_CHECK(totalNumRows += internalIDChunk.getNumValues());
        for (auto rowIdx = 0u; rowIdx < internalIDChunk.getNumValues(); rowIdx++) {
            const auto localRelOffset = internalIDChunk.getValue<offset_t>(rowIdx);
            const auto committedRelOffset = getCommittedOffset(localRelOffset, maxCommittedOffset);
            internalIDChunk.setValue<offset_t>(committedRelOffset, rowIdx);
        }

        internalIDChunk.setTableID(tableID);
    }
    DASSERT(totalNumRows == localNodeGroup.getNumRows());
}

offset_t RelTable::getCommittedOffset(offset_t uncommittedOffset, offset_t maxCommittedOffset) {
    return uncommittedOffset - StorageConstants::MAX_NUM_ROWS_IN_TABLE + maxCommittedOffset;
}

void RelTable::prepareCommitForNodeGroup(const Transaction* transaction,
    const std::vector<column_id_t>& columnIDs, const NodeGroup& localNodeGroup,
    CSRNodeGroup& csrNodeGroup, offset_t boundOffsetInGroup, const row_idx_vec_t& rowIndices,
    column_id_t skippedColumn) {
    for (const auto row : rowIndices) {
        auto [chunkedGroupIdx, rowInChunkedGroup] =
            StorageUtils::getQuotientRemainder(row, StorageConfig::CHUNKED_NODE_GROUP_CAPACITY);
        std::vector<const ColumnChunk*> chunks;
        const auto chunkedGroup = localNodeGroup.getChunkedNodeGroup(chunkedGroupIdx);
        for (auto i = 0u; i < chunkedGroup->getNumColumns(); i++) {
            if (i == skippedColumn) {
                continue;
            }
            chunks.push_back(&chunkedGroup->getColumnChunk(i));
        }
        csrNodeGroup.append(transaction, columnIDs, boundOffsetInGroup, chunks, rowInChunkedGroup,
            1 /*numRows*/);
    }
}

bool RelTable::checkpoint(main::ClientContext*, TableCatalogEntry* tableEntry,
    PageAllocator& pageAllocator, const Transaction* snapshotTxn, uint64_t epochWatermark,
    transaction_t retentionHorizon, const std::vector<transaction_t>& openStartTSs) {
    const auto effectiveEpoch =
        epochWatermark > 0 ? epochWatermark : changeEpoch.load(std::memory_order_acquire);
    if (effectiveEpoch <= lastCheckpointedEpoch) {
        return false;
    }
    // Deleted columns are vacuumed and not checkpointed or serialized.
    std::vector<column_id_t> columnIDs;
    columnIDs.push_back(0);
    for (auto& property : tableEntry->getProperties()) {
        columnIDs.push_back(tableEntry->getColumnID(property.getName()));
    }
    for (auto& directedRelData : directedRelData) {
        directedRelData->checkpoint(columnIDs, pageAllocator, snapshotTxn, retentionHorizon,
            openStartTSs);
    }
    lastCheckpointedEpoch = effectiveEpoch;
    return true;
}

row_idx_t RelTable::getNumTotalRows(const Transaction* transaction) {
    auto numLocalRows = 0u;
    if (auto localTable = transaction->getLocalStorage()->getLocalTable(*this)) {
        numLocalRows = localTable->getNumTotalRows();
    }
    return numLocalRows + nextRelOffset;
}

// COPY and checkpoint flush a persistent group before publishing it, so its CSR header chunks are
// on disk: read them through their columns.
static std::unique_ptr<ColumnChunkData> scanCSRHeaderColumn(MemoryManager& memoryManager,
    ColumnChunk& chunk, Column& column, offset_t numValues) {
    auto result = ColumnChunkFactory::createColumnChunkData(memoryManager, LogicalType::UINT64(),
        false /*enableCompression*/, StorageConfig::NODE_GROUP_SIZE, ResidencyState::IN_MEMORY,
        false /*hasNullData*/, false /*initializeToZero*/);
    ChunkState chunkState;
    chunk.initializeScanState(chunkState, &column);
    column.scan(chunkState, result.get(), 0 /*offsetInChunk*/, numValues);
    return result;
}

static uint64_t readCSRHeaderValue(ColumnChunk& chunk, Column& column, offset_t offset) {
    ChunkState chunkState;
    chunk.initializeScanState(chunkState, &column);
    const auto [segmentState, offsetInSegment] = chunkState.findSegment(offset);
    uint64_t value = 0;
    column.scanSegment(*segmentState, offsetInSegment, 1 /*length*/,
        reinterpret_cast<uint8_t*>(&value));
    return value;
}

// Rows [0, numRows) of the group, gap rows included, are all visible to the transaction: it sees no
// delete in the group and every insert in the range.
static bool allRowsVisible(const ChunkedNodeGroup& group, row_idx_t numRows,
    const Transaction* transaction) {
    if (!group.hasVersionInfo()) {
        return true;
    }
    const auto& versionInfo = *group.getVersionInfo();
    return !versionInfo.hasDeletions(transaction) &&
           versionInfo.getNumInsertedRows(transaction, 0, numRows) == numRows;
}

// Adds each node's rels in a persistent group that the transaction can see. A node's rels are the
// first `length` rows of its CSR region; the rest of the region is gap rows.
static void addPersistentDegrees(MemoryManager& memoryManager, const RelTableData& relTableData,
    ChunkedNodeGroup& group, const Transaction* transaction, std::vector<row_idx_t>& degrees,
    std::vector<row_idx_t>& deletedRows) {
    auto& csrHeader = group.cast<ChunkedCSRNodeGroup>().getCSRHeader();
    const auto numNodes = csrHeader.length->getNumValues();
    const auto lengths = scanCSRHeaderColumn(memoryManager, *csrHeader.length,
        *relTableData.getCSRLengthColumn(), numNodes);
    const auto* lengthData = reinterpret_cast<const uint64_t*>(lengths->getData());
    if (allRowsVisible(group, group.getNumRows(), transaction)) {
        for (offset_t i = 0; i < numNodes; ++i) {
            degrees[i] += lengthData[i];
        }
        return;
    }
    const auto offsets = scanCSRHeaderColumn(memoryManager, *csrHeader.offset,
        *relTableData.getCSROffsetColumn(), numNodes);
    const auto* endOffsets = reinterpret_cast<const uint64_t*>(offsets->getData());
    const auto& versionInfo = *group.getVersionInfo();
    const auto numRows = group.getNumRows();
    if (versionInfo.getNumInsertedRows(transaction, 0, numRows) == numRows) {
        // Only deletions hide rows: take each deleted row off the node whose region holds it. Gap
        // rows are never deleted.
        for (offset_t i = 0; i < numNodes; ++i) {
            degrees[i] += lengthData[i];
        }
        deletedRows.clear();
        versionInfo.getDeletedRows(transaction, 0, numRows, deletedRows);
        for (const auto row : deletedRows) {
            const auto node = std::upper_bound(endOffsets, endOffsets + numNodes, row) - endOffsets;
            if (static_cast<offset_t>(node) < numNodes) {
                degrees[node]--;
            }
        }
        return;
    }
    for (offset_t i = 0; i < numNodes; ++i) {
        const auto start = i == 0 ? 0 : endOffsets[i - 1];
        degrees[i] += versionInfo.getNumVisibleRows(transaction, start, lengthData[i]);
    }
}

// The rels in a persistent group that the transaction can see. count(*) needs only this total, so
// unlike addPersistentDegrees it takes deletions off without finding the node each belongs to.
static row_idx_t countVisiblePersistentRels(MemoryManager& memoryManager,
    const RelTableData& relTableData, ChunkedNodeGroup& group, const Transaction* transaction) {
    auto& csrHeader = group.cast<ChunkedCSRNodeGroup>().getCSRHeader();
    const auto numNodes = csrHeader.length->getNumValues();
    const auto lengths = scanCSRHeaderColumn(memoryManager, *csrHeader.length,
        *relTableData.getCSRLengthColumn(), numNodes);
    const auto* lengthData = reinterpret_cast<const uint64_t*>(lengths->getData());
    row_idx_t numRels = 0;
    for (offset_t i = 0; i < numNodes; ++i) {
        numRels += lengthData[i];
    }
    if (!group.hasVersionInfo()) {
        return numRels;
    }
    const auto& versionInfo = *group.getVersionInfo();
    const auto numRows = group.getNumRows();
    if (versionInfo.getNumInsertedRows(transaction, 0, numRows) == numRows) {
        // Gap rows are never deleted, so every deletion is one of the rels.
        return numRels - versionInfo.getNumDeletions(transaction, 0, numRows);
    }
    const auto offsets = scanCSRHeaderColumn(memoryManager, *csrHeader.offset,
        *relTableData.getCSROffsetColumn(), numNodes);
    const auto* endOffsets = reinterpret_cast<const uint64_t*>(offsets->getData());
    numRels = 0;
    for (offset_t i = 0; i < numNodes; ++i) {
        const auto start = i == 0 ? 0 : endOffsets[i - 1];
        numRels += versionInfo.getNumVisibleRows(transaction, start, lengthData[i]);
    }
    return numRels;
}

// Rows [0, n) of each pinned in-memory group are visible to the transaction, where n is the group's
// row count when it was found fully visible, else 0. The CSR index is live, so rows appended after
// that are checked one by one.
static std::vector<row_idx_t> getVisibleInMemPrefixes(const CSRNodeGroupScanPin& pin,
    const Transaction* transaction) {
    std::vector<row_idx_t> prefixes(pin.inMemGroups.size(), 0);
    for (auto i = 0u; i < pin.inMemGroups.size(); ++i) {
        if (!pin.inMemGroups[i]) {
            continue;
        }
        // An append stamps its rows' versions before it adds them to numRows, so the versions of
        // every row below this count are in place when they are checked.
        const auto numRows = pin.inMemGroups[i]->getNumRows();
        if (allRowsVisible(*pin.inMemGroups[i], numRows, transaction)) {
            prefixes[i] = numRows;
        }
    }
    return prefixes;
}

static row_idx_t countVisibleInMemRows(const CSRNodeGroupScanPin& pin,
    const std::vector<row_idx_t>& visiblePrefixes, const NodeCSRIndex& index,
    const Transaction* transaction) {
    auto isVisible = [&](row_idx_t rowIdx) {
        if (rowIdx == INVALID_ROW_IDX) {
            return false;
        }
        const auto [chunkIdx, rowInChunk] =
            StorageUtils::getQuotientRemainder(rowIdx, StorageConfig::CHUNKED_NODE_GROUP_CAPACITY);
        if (chunkIdx >= pin.inMemGroups.size() || !pin.inMemGroups[chunkIdx]) {
            return false;
        }
        if (rowInChunk < visiblePrefixes[chunkIdx]) {
            return true;
        }
        const auto& group = *pin.inMemGroups[chunkIdx];
        return group.isInserted(transaction, rowInChunk) &&
               !group.isDeleted(transaction, rowInChunk);
    };
    if (!index.isSequential) {
        row_idx_t count = 0;
        for (const auto rowIdx : index.rowIndices) {
            count += isVisible(rowIdx);
        }
        return count;
    }
    // A sequential index is {first row, number of rows}, and the rows are contiguous: take each
    // chunk's visible prefix at once.
    if (index.rowIndices.size() < 2) {
        return 0;
    }
    row_idx_t count = 0;
    auto row = index.rowIndices[0];
    auto remaining = index.rowIndices[1];
    while (remaining > 0) {
        const auto [chunkIdx, rowInChunk] =
            StorageUtils::getQuotientRemainder(row, StorageConfig::CHUNKED_NODE_GROUP_CAPACITY);
        const auto numInChunk =
            std::min(remaining, StorageConfig::CHUNKED_NODE_GROUP_CAPACITY - rowInChunk);
        row_idx_t numInPrefix = 0;
        if (chunkIdx < visiblePrefixes.size() && rowInChunk < visiblePrefixes[chunkIdx]) {
            numInPrefix = std::min(numInChunk, visiblePrefixes[chunkIdx] - rowInChunk);
        }
        count += numInPrefix;
        for (auto i = numInPrefix; i < numInChunk; ++i) {
            count += isVisible(row + i);
        }
        row += numInChunk;
        remaining -= numInChunk;
    }
    return count;
}

// One node's committed rels the transaction can see.
static row_idx_t getCommittedDegree(const RelTableData& relTableData, const CSRNodeGroup& nodeGroup,
    offset_t offsetInGroup, const Transaction* transaction) {
    const auto pin = nodeGroup.captureScanPin(transaction);
    row_idx_t count = 0;
    if (auto* persistentGroup = pin.persistent.get()) {
        auto& csrHeader = persistentGroup->cast<ChunkedCSRNodeGroup>().getCSRHeader();
        if (offsetInGroup < csrHeader.length->getNumValues()) {
            const auto length = readCSRHeaderValue(*csrHeader.length,
                *relTableData.getCSRLengthColumn(), offsetInGroup);
            if (!persistentGroup->hasVersionInfo()) {
                count += length;
            } else {
                const auto start = offsetInGroup == 0 ?
                                       0 :
                                       readCSRHeaderValue(*csrHeader.offset,
                                           *relTableData.getCSROffsetColumn(), offsetInGroup - 1);
                count += persistentGroup->getVersionInfo()->getNumVisibleRows(transaction, start,
                    length);
            }
        }
    }
    if (pin.index) {
        count += countVisibleInMemRows(pin, getVisibleInMemPrefixes(pin, transaction),
            pin.index->indices[offsetInGroup], transaction);
    }
    return count;
}

// Calls func(offset, degree) for every bound node with at least one rel, in ascending offset
// order. A degree counts the node's rels the transaction can see: persistent, committed in memory
// and, in a write transaction, uncommitted.
template<typename FUNC>
void RelTable::forEachDegree(const Transaction* transaction, RelDataDirection direction,
    FUNC&& func) {
    static_assert(std::is_same_v<DirectedCSRIndex::index_t, std::map<offset_t, row_idx_vec_t>>,
        "uncommitted rels are merged in offset order");
    static const DirectedCSRIndex::index_t noLocalRels;
    const DirectedCSRIndex::index_t* localRels = &noLocalRels;
    if (transaction->isWriteTransaction()) {
        if (auto* localTable = transaction->getLocalStorage()->getLocalTable(*this)) {
            localRels = &localTable->cast<LocalRelTable>().getCSRIndex(direction);
        }
    }
    auto localIt = localRels->begin();
    auto* relTableData = getDirectedTableData(direction);
    std::vector<row_idx_t> nodeGroupDegrees;
    std::vector<row_idx_t> deletedRows;
    for (node_group_idx_t nodeGroupIdx = 0; nodeGroupIdx < relTableData->getNumNodeGroups();
         nodeGroupIdx++) {
        auto* nodeGroup = relTableData->getNodeGroup(nodeGroupIdx);
        if (!nodeGroup) {
            continue;
        }
        auto groupStartOffset = StorageUtils::getStartOffsetOfNodeGroup(nodeGroupIdx);
        auto groupEndOffset = groupStartOffset + StorageConfig::NODE_GROUP_SIZE;
        nodeGroupDegrees.assign(StorageConfig::NODE_GROUP_SIZE, 0);
        const auto pin = nodeGroup->cast<CSRNodeGroup>().captureScanPin(transaction);
        if (pin.persistent) {
            addPersistentDegrees(*memoryManager, *relTableData, *pin.persistent, transaction,
                nodeGroupDegrees, deletedRows);
        }
        if (pin.index) {
            const auto visiblePrefixes = getVisibleInMemPrefixes(pin, transaction);
            for (offset_t i = 0; i < StorageConfig::NODE_GROUP_SIZE; ++i) {
                nodeGroupDegrees[i] +=
                    countVisibleInMemRows(pin, visiblePrefixes, pin.index->indices[i], transaction);
            }
        }
        for (; localIt != localRels->end() && localIt->first < groupEndOffset; ++localIt) {
            const auto& [nodeOffset, rowIndices] = *localIt;
            if (nodeOffset >= groupStartOffset) {
                nodeGroupDegrees[nodeOffset - groupStartOffset] += rowIndices.size();
            } else if (!rowIndices.empty()) {
                // Belongs to a node group skipped above as null, so it has uncommitted rels only.
                func(nodeOffset, rowIndices.size());
            }
        }
        for (offset_t i = 0; i < StorageConfig::NODE_GROUP_SIZE; ++i) {
            if (nodeGroupDegrees[i] > 0) {
                func(groupStartOffset + i, nodeGroupDegrees[i]);
            }
        }
    }
    for (; localIt != localRels->end(); ++localIt) {
        const auto& [nodeOffset, rowIndices] = *localIt;
        if (!rowIndices.empty()) {
            func(nodeOffset, rowIndices.size());
        }
    }
}

std::vector<std::pair<offset_t, row_idx_t>> RelTable::getDegreeEntries(
    const Transaction* transaction, RelDataDirection direction) {
    std::vector<std::pair<offset_t, row_idx_t>> result;
    forEachDegree(transaction, direction,
        [&](offset_t offset, row_idx_t degree) { result.emplace_back(offset, degree); });
    return result;
}

RelDegreeSummary RelTable::getDegreeSummary(const Transaction* transaction,
    RelDataDirection direction) {
    RelDegreeSummary summary;
    forEachDegree(transaction, direction, [&](offset_t, row_idx_t degree) {
        summary.numActiveBoundNodes++;
        summary.totalDegree += degree;
        summary.maxDegree = std::max(summary.maxDegree, degree);
    });
    return summary;
}

std::vector<std::pair<offset_t, row_idx_t>> RelTable::getTopKDegrees(const Transaction* transaction,
    RelDataDirection direction, idx_t k) {
    if (k == 0) {
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

    forEachDegree(transaction, direction, [&](offset_t offset, row_idx_t degree) {
        const degree_entry_t entry{offset, degree};
        if (heap.size() < k) {
            heap.push(entry);
        } else if (better(entry, heap.top())) {
            heap.pop();
            heap.push(entry);
        }
    });

    std::vector<degree_entry_t> result;
    while (!heap.empty()) {
        result.push_back(heap.top());
        heap.pop();
    }
    std::sort(result.begin(), result.end(), better);
    return result;
}

row_idx_t RelTable::getNumVisibleRels(const Transaction* transaction, RelDataDirection direction) {
    auto* relTableData = getDirectedTableData(direction);
    row_idx_t numRels = 0;
    for (node_group_idx_t nodeGroupIdx = 0; nodeGroupIdx < relTableData->getNumNodeGroups();
         nodeGroupIdx++) {
        auto* nodeGroup = relTableData->getNodeGroup(nodeGroupIdx);
        if (!nodeGroup) {
            continue;
        }
        const auto pin = nodeGroup->cast<CSRNodeGroup>().captureScanPin(transaction);
        if (pin.persistent) {
            numRels += countVisiblePersistentRels(*memoryManager, *relTableData, *pin.persistent,
                transaction);
        }
        // In-memory groups have no gap rows, so their visible rows are the rels. The degree paths
        // walk the CSR index instead, since they need each row's node.
        for (const auto& inMemGroup : pin.inMemGroups) {
            numRels += inMemGroup->getNumVisibleRows(transaction);
        }
    }
    if (transaction->isWriteTransaction()) {
        if (auto* localTable = transaction->getLocalStorage()->getLocalTable(*this)) {
            for (const auto& [_, rowIndices] :
                localTable->cast<LocalRelTable>().getCSRIndex(direction)) {
                numRels += rowIndices.size();
            }
        }
    }
    return numRels;
}

row_idx_t RelTable::getNumActiveBoundNodes(const Transaction* transaction,
    RelDataDirection direction) {
    return getDegreeSummary(transaction, direction).numActiveBoundNodes;
}

row_idx_t RelTable::getDegreeForOffset(const Transaction* transaction, RelDataDirection direction,
    offset_t nodeOffset) {
    const auto nodeGroupIdx = StorageUtils::getNodeGroupIdx(nodeOffset);
    const auto offsetInGroup = nodeOffset - StorageUtils::getStartOffsetOfNodeGroup(nodeGroupIdx);
    auto* relTableData = getDirectedTableData(direction);
    row_idx_t count = 0;
    if (nodeGroupIdx < relTableData->getNumNodeGroups()) {
        if (auto* nodeGroup = relTableData->getNodeGroup(nodeGroupIdx)) {
            count += getCommittedDegree(*relTableData, nodeGroup->cast<CSRNodeGroup>(),
                offsetInGroup, transaction);
        }
    }
    if (transaction->isWriteTransaction()) {
        if (auto* localTable = transaction->getLocalStorage()->getLocalTable(*this)) {
            auto& localCSRIndex = localTable->cast<LocalRelTable>().getCSRIndex(direction);
            if (auto it = localCSRIndex.find(nodeOffset); it != localCSRIndex.end()) {
                count += it->second.size();
            }
        }
    }
    return count;
}

void RelTable::serialize(Serializer& ser) const {
    ser.writeDebuggingInfo("next_rel_offset");
    ser.write<offset_t>(nextRelOffset);
    for (auto& directedRelData : directedRelData) {
        directedRelData->serialize(ser);
    }
}

void RelTable::deserialize(main::ClientContext*, StorageManager*, Deserializer& deSer) {
    std::string key;
    deSer.validateDebuggingInfo(key, "next_rel_offset");
    deSer.deserializeValue<offset_t>(nextRelOffset);
    for (auto i = 0u; i < directedRelData.size(); i++) {
        directedRelData[i]->deserialize(deSer, *memoryManager);
    }
}

} // namespace storage
} // namespace lbug

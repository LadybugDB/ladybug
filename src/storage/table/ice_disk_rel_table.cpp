#include "storage/table/ice_disk_rel_table.h"

#include <cstring>
#include <filesystem>
#include <unordered_map>

#include "storage/storage_manager.h"
#include "storage/table/csr_node_group.h"
#include "transaction/transaction.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/runtime.h"

using namespace lbug::common;
using namespace lbug::transaction;
using namespace lbug::catalog;

namespace lbug {
namespace storage {

void IceDiskRelTableScanState::setToTable(const Transaction* transaction, Table* table_,
    std::vector<common::column_id_t> columnIDs_,
    std::vector<ColumnPredicateSet> columnPredicateSets_,
    common::RelDataDirection direction_) {
    // Call base class implementation but skip local table setup
    TableScanState::setToTable(transaction, table_, std::move(columnIDs_),
        std::move(columnPredicateSets_));
    direction = direction_;
}

void IceDiskRelTableScanState::initializeIndicesReader(Transaction* transaction) {
    if (!indicesReader) {
        std::lock_guard<std::mutex> lock(mtx);

        if(!indicesReader) { // Double-checked locking to avoid redundant initialization
            auto* iceDiskRelTable = static_cast<IceDiskRelTable*>(table);
            indicesReader = std::make_unique<processor::ParquetReader>(iceDiskRelTable->getIndicesFilePath(), std::vector<bool>(), transaction->getClientContext());
        }
    }
}

IceDiskRelTable::IceDiskRelTable(RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
    common::table_id_t toTableID, const StorageManager* storageManager,
    MemoryManager* memoryManager)
    : RelTable{relGroupEntry, fromTableID, toTableID, storageManager, memoryManager},
      relGroupCatalogEntry{relGroupEntry} {
    if (relGroupEntry->getIndicesPath().empty()) {
        throw RuntimeException("Indices file path is empty for icebug-disk-backed rel table");
    }

    if (relGroupEntry->getIndptrPath().empty()) {
        throw RuntimeException("Indptr file path is empty for icebug-disk-backed rel table");
    }

    indicesFilePath = relGroupEntry->getIndicesPath();
    indptrFilePath = relGroupEntry->getIndptrPath();
}

void IceDiskRelTable::initializeScanCoordination(Transaction* transaction) {
    indicesRowGroupStartOffsets = getIndicesRowGroupStartOffsets(transaction);
    indptrData = readIndptrData(transaction);
}

void IceDiskRelTable::initScanState(Transaction* transaction, TableScanState& scanState,
    bool resetCachedBoundNodeSelVec) const {
    auto& relScanState = scanState.cast<RelTableScanState>();
    
    // For morsel-driven parallelism, each scan state maintains its own bound node processing state
    // No shared state needed between threads
    if (resetCachedBoundNodeSelVec) {
        // Copy the cached bound node selection vector from the scan state
        copyCachedBoundNodeSelVector(relScanState);
    }

    auto& iceDiskScanState = static_cast<IceDiskRelTableScanState&>(relScanState);
    iceDiskScanState.initializeIndicesReader(transaction);
    iceDiskScanState.currentRowGroupIdx = 0;
}

bool IceDiskRelTable::scanInternal(Transaction* transaction, TableScanState& scanState) {
    auto& iceDiskScanState = static_cast<IceDiskRelTableScanState&>(scanState);

    scanState.resetOutVectors();

    // Check if we have any row groups left to process
    if (iceDiskScanState.currentRowGroupIdx >= indicesRowGroupStartOffsets.size()) {
        // No more row groups to process
        auto newSelVector = std::make_shared<SelectionVector>(0);
        iceDiskScanState.outState->setSelVector(newSelVector);
        return false;
    }

    // Process the current row group
    std::vector<uint64_t> rowGroupsToProcess = {iceDiskScanState.currentRowGroupIdx};

    // Create a set of bound node IDs for fast lookup
    std::unordered_set<common::offset_t> boundNodeOffsets;
    for (size_t i = 0; i < iceDiskScanState.cachedBoundNodeSelVector.getSelSize(); ++i) {
        common::sel_t boundNodeIdx = iceDiskScanState.cachedBoundNodeSelVector[i];
        const auto boundNodeID = iceDiskScanState.nodeIDVector->getValue<nodeID_t>(boundNodeIdx);
        boundNodeOffsets.insert(boundNodeID.offset);
    }

    // Scan the current row group and collect relationships for bound nodes
    bool hasData = scanRowGroupForBoundNodes(transaction, iceDiskScanState, rowGroupsToProcess,
        boundNodeOffsets);

    // Move to next row group for next call
    iceDiskScanState.currentRowGroupIdx++;

    return hasData;
}

bool IceDiskRelTable::scanRowGroupForBoundNodes(Transaction* transaction,
    IceDiskRelTableScanState& iceDiskScanState, const std::vector<std::size_t>& rowGroupsToProcess,
    const std::unordered_set<common::offset_t>& boundNodeOffsets) {
    if (!iceDiskScanState.indicesReader) {
        return false;
    }

    // Initialize scan state for the assigned row groups
    auto context = transaction->getClientContext();
    auto vfs = VirtualFileSystem::GetUnsafe(*context);
    iceDiskScanState.indicesReader->initializeScan(*iceDiskScanState.parquetScanState,
        rowGroupsToProcess, vfs);

    // Create DataChunk matching the indices parquet file schema
    auto numIndicesColumns = iceDiskScanState.indicesReader->getNumColumns();
    DataChunk indicesChunk(numIndicesColumns);

    // Insert value vectors for all columns in the parquet file
    auto memoryManager = MemoryManager::Get(*context);
    for (uint32_t colIdx = 0; colIdx < numIndicesColumns; ++colIdx) {
        const auto& columnTypeRef = iceDiskScanState.indicesReader->getColumnType(colIdx);
        auto columnType = columnTypeRef.copy();
        auto vector = std::make_shared<ValueVector>(std::move(columnType), memoryManager);
        indicesChunk.insert(colIdx, vector);
    }

    // Scan the row groups and collect relationships for bound nodes.
    const auto isFwd = iceDiskScanState.direction != RelDataDirection::BWD;
    uint64_t totalRowsCollected = 0;
    uint64_t currentGlobalRowIdx = 0;

    // Calculate the starting global row index for the first row group
    if (!rowGroupsToProcess.empty()) {
        currentGlobalRowIdx = indicesRowGroupStartOffsets[rowGroupsToProcess[0]];
    }

    while (totalRowsCollected < IceDiskRelTable::scanRowGroupBatchSize &&
           iceDiskScanState.indicesReader->scanInternal(*iceDiskScanState.parquetScanState,
               indicesChunk)) {

        auto selSize = indicesChunk.state->getSelVector().getSelSize();

        for (size_t i = 0; i < selSize && totalRowsCollected < IceDiskRelTable::scanRowGroupBatchSize;
             ++i, ++currentGlobalRowIdx) {
            // Find which source node this row belongs to.
            const auto sourceNodeOffset = findSourceNodeForRow(currentGlobalRowIdx);

            // Column 0 in indices file is the destination node offset.
            const auto dstOffset = indicesChunk.getValueVector(0).getValue<std::size_t>(i);
            const auto boundOffset = isFwd ? sourceNodeOffset : dstOffset;
            
            // not a bound node, skip
            if (boundNodeOffsets.find(boundOffset) == boundNodeOffsets.end()) {
                continue;
            }

            const auto nbrOffset = isFwd ? dstOffset : sourceNodeOffset;
            const auto nbrTableID = isFwd ? getToNodeTableID() : getFromNodeTableID();
            auto nbrNodeID = internalID_t(nbrOffset, nbrTableID);

            // outputVectors[0] is the neighbor node ID, if requested.
            if (!iceDiskScanState.outputVectors.empty()) {
                iceDiskScanState.outputVectors[0]->setValue(totalRowsCollected, nbrNodeID);
            }

            // If there are additional columns (e.g., weight), copy them to subsequent output
            // vectors These are property columns and should have matching types
            for (uint32_t colIdx = 1;
                 colIdx < numIndicesColumns && colIdx < iceDiskScanState.outputVectors.size();
                 ++colIdx) {
                iceDiskScanState.outputVectors[colIdx]->copyFromVectorData(totalRowsCollected,
                    &indicesChunk.getValueVector(colIdx), i);
            }

            totalRowsCollected++;
        }
    }

    // No data found
    if (totalRowsCollected <= 0) {
        auto selVector = std::make_shared<SelectionVector>(0);
        iceDiskScanState.outState->setSelVector(selVector);
        return false;
    }

    auto selVector = std::make_shared<SelectionVector>(totalRowsCollected);
    selVector->setToUnfiltered(totalRowsCollected);
    iceDiskScanState.outState->setSelVector(selVector);

    return true;
}

common::row_idx_t IceDiskRelTable::getNumTotalRows(const Transaction* transaction) {
    auto context = transaction->getClientContext();
    auto resolvedPath = VirtualFileSystem::resolvePath(context, indicesFilePath);
    std::vector<bool> dummySkips;
    processor::ParquetReader reader(resolvedPath, dummySkips, context);
    return reader.getMetadata()->num_rows;
}

std::vector<std::size_t> IceDiskRelTable::getIndicesRowGroupStartOffsets(const transaction::Transaction* transaction) const {
    auto context = transaction->getClientContext();
    auto resolvedPath = VirtualFileSystem::resolvePath(context, indicesFilePath);
    processor::ParquetReader reader(resolvedPath, std::vector<bool>(), context);
    
    auto metadata = reader.getMetadata();
    std::vector<std::size_t> startOffsets;
    std::size_t currentOffset = 0;
    
    for (auto i = 0u; i < metadata->row_groups.size(); ++i) {
        startOffsets.push_back(currentOffset);
        currentOffset += metadata->row_groups[i].num_rows;
    }

    return startOffsets;
}

std::vector<std::size_t> IceDiskRelTable::readIndptrData(Transaction* transaction) const {
    auto context = transaction->getClientContext();
    auto vfs = VirtualFileSystem::GetUnsafe(*context);
    auto resolvedPath = VirtualFileSystem::resolvePath(context, indptrFilePath);
    auto indptrReader = std::make_unique<processor::ParquetReader>(resolvedPath, std::vector<bool>(), context);
    processor::ParquetReaderScanState scanState;
    std::vector<uint64_t> groupsToRead;
    std::vector<std::size_t> indptrData;

    for (uint64_t i = 0; i < indptrReader->getMetadata()->row_groups.size(); ++i) {
        groupsToRead.push_back(i);
    }

    indptrReader->initializeScan(scanState, groupsToRead, vfs);

    // Check if the indptr file has any columns after scan initialization
    auto numColumns = indptrReader->getNumColumns();
    if (numColumns == 0) {
        throw RuntimeException("Indptr parquet file has no columns");
    }

    // Validate column type for indptr
    const auto& indptrType = indptrReader->getColumnType(0);
    if (!LogicalTypeUtils::isIntegral(indptrType.getLogicalTypeID())) {
        throw RuntimeException(
            "Indptr parquet file column must be integer type (column 0)");
    }

    DataChunk dataChunk(1);
    const auto& columnTypeRef = indptrReader->getColumnType(0);
    auto columnType = columnTypeRef.copy();
    auto vector = std::make_shared<ValueVector>(std::move(columnType), MemoryManager::Get(*context));
    dataChunk.insert(0, vector);
    
    while (indptrReader->scanInternal(scanState, dataChunk)) {
        auto selVector = dataChunk.state->getSelVectorShared();
        auto selSize = selVector->getSelSize();
        auto& valVector = dataChunk.getValueVector(0);

        for (std::size_t i = 0; i < selSize; ++i) {
            auto value = valVector.getValue<std::size_t>((*selVector)[i]);
            indptrData.push_back(value);
        }
    }

    return indptrData;
}

void IceDiskRelTable::copyCachedBoundNodeSelVector(RelTableScanState& relScanState) const {
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

std::size_t IceDiskRelTable::findSourceNodeForRow(std::size_t globalRowIdx) const {
    if (indptrData.empty()) {
        throw RuntimeException("Indptr data not loaded for CSR format");
    }

    // Binary search to find the source node
    // indptrData[i] contains the start row index for source node i
    // Find the largest i where indptrData[i] <= globalRowIdx
    auto it = std::upper_bound(indptrData.begin(), indptrData.end(), globalRowIdx);
    if (it == indptrData.begin()) {
        throw RuntimeException("Invalid global row index: " + std::to_string(globalRowIdx));
    }
    --it;
    return static_cast<std::size_t>(std::distance(indptrData.begin(), it));
}

} // namespace storage
} // namespace lbug

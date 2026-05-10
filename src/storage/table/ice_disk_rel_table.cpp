#include "storage/table/ice_disk_rel_table.h"

#include <filesystem>

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/data_chunk/sel_vector.h"
#include "common/exception/runtime.h"
#include "common/file_system/virtual_file_system.h"
#include "main/client_context.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"
#include "storage/storage_manager.h"
#include "storage/table/ice_disk_utils.h"
#include "transaction/transaction.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::processor;
using namespace lbug::transaction;

namespace lbug {
namespace storage {

void IceDiskRelTableScanState::setToTable(const Transaction* transaction, Table* table_,
    std::vector<column_id_t> columnIDs_, std::vector<ColumnPredicateSet> columnPredicateSets_,
    RelDataDirection direction_) {
    // Call base class implementation but skip local table setup
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
    // IceDiskRelTable does not support local storage, so we skip the local table initialization
}

IceDiskRelTable::IceDiskRelTable(RelGroupCatalogEntry* relGroupEntry, table_id_t fromTableID,
    table_id_t toTableID, const StorageManager* storageManager, MemoryManager* memoryManager)
    : ColumnarRelTableBase{relGroupEntry, fromTableID, toTableID, storageManager, memoryManager} {
    const auto base = IceDiskUtils::getBasePath(relGroupEntry->getStorage());
    auto paths = IceDiskUtils::constructCSRPaths(base, relGroupEntry->getName(), ".parquet");

    const auto dbDir =
        std::filesystem::path(storageManager->getDatabasePath()).parent_path().string();
    IceDiskUtils::checkVersionCompatibility(storageManager->getVFS(), dbDir, paths.indices);
    IceDiskUtils::checkVersionCompatibility(storageManager->getVFS(), dbDir, paths.indptr);

    indicesFilePath = paths.indices;
    indptrFilePath = paths.indptr;
}

void IceDiskRelTable::initScanState(Transaction* transaction, TableScanState& scanState,
    bool resetCachedBoundNodeSelVec) const {
    // For  tables, we create our own scan state
    auto& relScanState = scanState.cast<RelTableScanState>();
    relScanState.source = TableScanSource::COMMITTED;
    relScanState.nodeGroup = nullptr;
    relScanState.nodeGroupIdx = INVALID_NODE_GROUP_IDX;

    // Initialize ParquetReaders for this scan state (per-thread)
    auto& iceDiskScanState = static_cast<IceDiskRelTableScanState&>(relScanState);

    // Initialize readers if not already done for this scan state
    if (!iceDiskScanState.indicesReader) {
        std::vector<bool> columnSkips; // Read all columns
        auto context = transaction->getClientContext();
        auto resolvedPath = VirtualFileSystem::resolvePath(context, indicesFilePath);
        iceDiskScanState.indicesReader =
            std::make_unique<ParquetReader>(resolvedPath, columnSkips, context);
    }
    if (!indptrFilePath.empty() && !iceDiskScanState.indptrReader) {
        std::vector<bool> columnSkips; // Read all columns
        auto context = transaction->getClientContext();
        auto resolvedPath = VirtualFileSystem::resolvePath(context, indptrFilePath);
        iceDiskScanState.indptrReader =
            std::make_unique<ParquetReader>(resolvedPath, columnSkips, context);
    }

    // Load shared indptr data - thread-safe to read
    if (!indptrFilePath.empty()) {
        loadIndptrData(transaction);
    }

    // For morsel-driven parallelism, each scan state maintains its own bound node processing state
    // No shared state needed between threads
    if (resetCachedBoundNodeSelVec) {
        // Copy the cached bound node selection vector from the scan state
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

    // Initialize row group ranges for morsel-driven parallelism
    // For now, assign all row groups to this scan state (will be partitioned by the scan operator)
    iceDiskScanState.currentRowGroup = 0;
    iceDiskScanState.endRowGroup =
        iceDiskScanState.indicesReader ? iceDiskScanState.indicesReader->getNumRowGroups() : 0;
}

void IceDiskRelTable::initializeParquetReaders(Transaction* transaction) const {
    if (!indicesReader) {
        std::lock_guard lock(parquetReaderMutex);
        if (!indicesReader) {
            std::vector<bool> columnSkips; // Read all columns
            auto context = transaction->getClientContext();
            auto resolvedPath = VirtualFileSystem::resolvePath(context, indicesFilePath);
            indicesReader = std::make_unique<ParquetReader>(resolvedPath, columnSkips, context);
        }
    }
}

void IceDiskRelTable::initializeIndptrReader(Transaction* transaction) const {
    if (!indptrFilePath.empty() && !indptrReader) {
        std::lock_guard lock(parquetReaderMutex);
        if (!indptrReader) {
            std::vector<bool> columnSkips; // Read all columns
            auto context = transaction->getClientContext();
            auto resolvedPath = VirtualFileSystem::resolvePath(context, indptrFilePath);
            indptrReader = std::make_unique<ParquetReader>(resolvedPath, columnSkips, context);
        }
    }
}

void IceDiskRelTable::loadIndptrData(Transaction* transaction) const {
    if (indptrData.empty() && !indptrFilePath.empty()) {
        std::lock_guard lock(indptrDataMutex);
        if (indptrData.empty()) {
            initializeIndptrReader(transaction);
            if (!indptrReader)
                return;

            // Initialize scan to populate column types
            auto context = transaction->getClientContext();
            auto vfs = VirtualFileSystem::GetUnsafe(*context);
            std::vector<uint64_t> groupsToRead;
            for (uint64_t i = 0; i < indptrReader->getNumRowGroups(); ++i) {
                groupsToRead.push_back(i);
            }

            ParquetReaderScanState scanState;
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

            // Read the indptr column
            DataChunk dataChunk(1);

            // Now get the column type after scan is initialized
            const auto& columnTypeRef = indptrReader->getColumnType(0);
            auto columnType = columnTypeRef.copy();
            auto vector = std::make_shared<ValueVector>(std::move(columnType));
            dataChunk.insert(0, vector);

            // Read all indptr values
            while (indptrReader->scanInternal(scanState, dataChunk)) {
                auto selSize = dataChunk.state->getSelVector().getSelSize();
                for (size_t i = 0; i < selSize; ++i) {
                    auto value = dataChunk.getValueVector(0).getValue<common::offset_t>(i);
                    indptrData.push_back(value);
                }
            }
        }
    }
}

bool IceDiskRelTable::scanInternal(Transaction* transaction, TableScanState& scanState) {
    auto& relScanState = scanState.cast<RelTableScanState>();

    // Get the IceDiskRelTableScanState
    auto& iceDiskScanState = static_cast<IceDiskRelTableScanState&>(relScanState);

    // Load shared indptr data - thread-safe to read
    if (!indptrFilePath.empty()) {
        loadIndptrData(transaction);
    }

    // True morsel-driven parallelism: each scan state processes its assigned row groups
    // Process all row groups assigned to this scan state, collecting relationships for bound nodes
    return scanInternalByRowGroups(transaction, iceDiskScanState);
}

bool IceDiskRelTable::scanInternalByRowGroups(Transaction* transaction,
    IceDiskRelTableScanState& iceDiskScanState) {
    // True morsel-driven parallelism: process assigned row groups and collect relationships for
    // bound nodes

    // Check if we have any row groups left to process
    if (iceDiskScanState.currentRowGroup >= iceDiskScanState.endRowGroup) {
        // No more row groups to process
        iceDiskScanState.outState->getSelVectorUnsafe().setToFiltered(0);
        return false;
    }

    // Process the current row group
    std::vector<uint64_t> rowGroupsToProcess = {iceDiskScanState.currentRowGroup};

    // Create a set of bound node IDs for fast lookup
    std::unordered_map<common::offset_t, common::sel_t> boundNodeOffsets;
    for (size_t i = 0; i < iceDiskScanState.cachedBoundNodeSelVector.getSelSize(); ++i) {
        common::sel_t boundNodeIdx = iceDiskScanState.cachedBoundNodeSelVector[i];
        const auto boundNodeID = iceDiskScanState.nodeIDVector->getValue<nodeID_t>(boundNodeIdx);
        boundNodeOffsets.insert({boundNodeID.offset, boundNodeIdx});
    }

    // Scan the current row group and collect relationships for bound nodes
    bool hasData = scanRowGroupForBoundNodes(transaction, iceDiskScanState, rowGroupsToProcess,
        boundNodeOffsets);

    // Move to next row group for next call
    iceDiskScanState.currentRowGroup++;

    return hasData;
}

common::offset_t IceDiskRelTable::findSourceNodeForRow(common::offset_t globalRowIdx) const {
    // Use base class helper for binary search
    return findSourceNodeForRowInternal(globalRowIdx, indptrData);
}

bool IceDiskRelTable::scanRowGroupForBoundNodes(Transaction* transaction,
    IceDiskRelTableScanState& iceDiskScanState, const std::vector<uint64_t>& rowGroupsToProcess,
    const std::unordered_map<common::offset_t, common::sel_t>& boundNodeOffsets) {

    // Initialize readers if needed
    initializeParquetReaders(transaction);

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
    const uint64_t maxRowsPerCall = DEFAULT_VECTOR_CAPACITY;
    uint64_t currentGlobalRowIdx = 0;
    auto activeBoundSelPos = INVALID_SEL;
    auto activeBoundOffset = INVALID_OFFSET;
    auto hasActiveBound = false;

    // Calculate the starting global row index for the first row group
    if (!rowGroupsToProcess.empty()) {
        auto metadata = iceDiskScanState.indicesReader->getMetadata();
        for (uint64_t rgIdx = 0; rgIdx < rowGroupsToProcess[0]; ++rgIdx) {
            currentGlobalRowIdx += metadata->row_groups[rgIdx].num_rows;
        }
    }

    while (totalRowsCollected < maxRowsPerCall &&
           iceDiskScanState.indicesReader->scanInternal(*iceDiskScanState.parquetScanState,
               indicesChunk)) {

        auto selSize = indicesChunk.state->getSelVector().getSelSize();

        for (size_t i = 0; i < selSize && totalRowsCollected < maxRowsPerCall;
             ++i, ++currentGlobalRowIdx) {
            // Find which source node this row belongs to.
            const auto sourceNodeOffset = findSourceNodeForRow(currentGlobalRowIdx);
            if (sourceNodeOffset == common::INVALID_OFFSET) {
                continue; // Invalid row
            }

            // Column 0 in indices file is the destination node offset.
            const auto dstOffset = indicesChunk.getValueVector(0).getValue<common::offset_t>(i);
            const auto boundOffset = isFwd ? sourceNodeOffset : dstOffset;
            if (boundNodeOffsets.find(boundOffset) == boundNodeOffsets.end()) {
                continue; // Not a bound node, skip
            }

            if (!hasActiveBound) {
                hasActiveBound = true;
                activeBoundOffset = boundOffset;
                activeBoundSelPos = boundNodeOffsets.at(boundOffset);
            } else if (boundOffset != activeBoundOffset) {
                break;
            }

            // This row belongs to a bound node, collect the relationship

            const auto nbrOffset = isFwd ? dstOffset : sourceNodeOffset;
            const auto nbrTableID = isFwd ? getToNodeTableID() : getFromNodeTableID();
            auto nbrNodeID = internalID_t(nbrOffset, nbrTableID);

            // outputVectors[0] is the neighbor node ID, if requested.
            if (!iceDiskScanState.outputVectors.empty()) {
                iceDiskScanState.outputVectors[0]->setValue(totalRowsCollected, nbrNodeID);
            }

            // Copy edge properties to output vectors.
            // Catalog col IDs: NBR_ID=0, REL_ID=1 (virtual), user props=2,3,...
            // Parquet cols:     target=0,              user props=1,2,...
            // So parquet_col = catalog_col_id - 1 for user properties.
            for (uint64_t outCol = 1; outCol < iceDiskScanState.outputVectors.size(); ++outCol) {
                if (outCol >= iceDiskScanState.columnIDs.size()) {
                    continue;
                }
                const auto colID = iceDiskScanState.columnIDs[outCol];
                if (colID == INVALID_COLUMN_ID || colID == ROW_IDX_COLUMN_ID ||
                    colID == NBR_ID_COLUMN_ID) {
                    continue;
                }
                if (colID == REL_ID_COLUMN_ID) {
                    // REL_ID is not stored in parquet; synthesize from the global row index.
                    iceDiskScanState.outputVectors[outCol]->setValue<internalID_t>(
                        totalRowsCollected, internalID_t{currentGlobalRowIdx, getTableID()});
                    continue;
                }

                iceDiskScanState.outputVectors[outCol]->copyFromVectorData(totalRowsCollected,
                    &indicesChunk.getValueVector(colID - 1), i);
            }

            totalRowsCollected++;
        }
    }

    // Set up the output state
    if (totalRowsCollected > 0) {
        auto& selVector = iceDiskScanState.outState->getSelVectorUnsafe();
        selVector.setToFiltered(totalRowsCollected);
        for (uint64_t i = 0; i < totalRowsCollected; ++i) {
            selVector[i] = i;
        }
        iceDiskScanState.setNodeIDVectorToFlat(activeBoundSelPos);

        return true;
    } else {
        // No data found
        iceDiskScanState.outState->getSelVectorUnsafe().setToFiltered(0);
        return false;
    }
}

row_idx_t IceDiskRelTable::getTotalRowCount(const Transaction* transaction) const {
    initializeParquetReaders(const_cast<Transaction*>(transaction));
    if (!indicesReader) {
        return 0;
    }
    auto metadata = indicesReader->getMetadata();
    return metadata ? metadata->num_rows : 0;
}

} // namespace storage
} // namespace lbug

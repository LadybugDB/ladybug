#include "storage/table/parquet_rel_table.h"

#include <thread>

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/data_chunk/sel_vector.h"
#include "common/exception/runtime.h"
#include "common/file_system/virtual_file_system.h"
#include "main/client_context.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"
#include "storage/storage_manager.h"
#include "transaction/transaction.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::processor;
using namespace lbug::transaction;

namespace lbug {
namespace storage {

void ParquetRelTableScanState::setToTable(const Transaction* transaction, Table* table_,
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
    // ParquetRelTable does not support local storage, so we skip the local table initialization
}

ParquetRelTable::ParquetRelTable(RelGroupCatalogEntry* relGroupEntry, table_id_t fromTableID,
    table_id_t toTableID, const StorageManager* storageManager, MemoryManager* memoryManager)
    : RelTable{relGroupEntry, fromTableID, toTableID, storageManager, memoryManager},
      relGroupEntry{relGroupEntry} {
    std::string storage = relGroupEntry->getStorage();
    if (storage.empty()) {
        throw RuntimeException("Parquet file path is empty for parquet-backed rel table");
    }

    // New prefix format: "prefix" which expands to:
    // prefix_node_mapping.parquet, prefix_indices.parquet, prefix_indptr.parquet
    std::string prefix = storage;
    nodeMappingFilePath = prefix + "_node_mapping.parquet";
    indicesFilePath = prefix + "_indices.parquet";
    indptrFilePath = prefix + "_indptr.parquet";
}

void ParquetRelTable::initScanState(Transaction* transaction, TableScanState& scanState,
    bool resetCachedBoundNodeSelVec) const {
    // For parquet tables, we create our own scan state
    auto& relScanState = scanState.cast<RelTableScanState>();
    relScanState.source = TableScanSource::COMMITTED;
    relScanState.nodeGroup = nullptr;
    relScanState.nodeGroupIdx = INVALID_NODE_GROUP_IDX;

    // Initialize ParquetReaders for this scan state (per-thread)
    auto& parquetRelScanState = static_cast<ParquetRelTableScanState&>(relScanState);

    // Initialize readers if not already done for this scan state
    if (!parquetRelScanState.nodeMappingReader) {
        std::vector<bool> columnSkips; // Read all columns
        auto context = transaction->getClientContext();
        parquetRelScanState.nodeMappingReader =
            std::make_unique<ParquetReader>(nodeMappingFilePath, columnSkips, context);
    }
    if (!parquetRelScanState.indicesReader) {
        std::vector<bool> columnSkips; // Read all columns
        auto context = transaction->getClientContext();
        parquetRelScanState.indicesReader =
            std::make_unique<ParquetReader>(indicesFilePath, columnSkips, context);
    }
    if (!indptrFilePath.empty() && !parquetRelScanState.indptrReader) {
        std::vector<bool> columnSkips; // Read all columns
        auto context = transaction->getClientContext();
        parquetRelScanState.indptrReader =
            std::make_unique<ParquetReader>(indptrFilePath, columnSkips, context);
    }

    // Load shared data (node mapping and indptr) - these are thread-safe to read
    loadNodeMappingData(transaction);
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
    parquetRelScanState.startRowGroup = 0;
    parquetRelScanState.endRowGroup = parquetRelScanState.indicesReader ?
                                          parquetRelScanState.indicesReader->getNumRowsGroups() :
                                          0;
    parquetRelScanState.currentRowGroup = parquetRelScanState.startRowGroup;
    parquetRelScanState.nextRowToProcess = 0;
}

void ParquetRelTable::initializeParquetReaders(Transaction* transaction) const {
    if (!nodeMappingReader || !indicesReader) {
        std::lock_guard lock(parquetReaderMutex);
        if (!nodeMappingReader) {
            std::vector<bool> columnSkips; // Read all columns
            auto context = transaction->getClientContext();
            nodeMappingReader =
                std::make_unique<ParquetReader>(nodeMappingFilePath, columnSkips, context);
        }
        if (!indicesReader) {
            std::vector<bool> columnSkips; // Read all columns
            auto context = transaction->getClientContext();
            indicesReader = std::make_unique<ParquetReader>(indicesFilePath, columnSkips, context);
        }
    }
}

void ParquetRelTable::initializeIndptrReader(Transaction* transaction) const {
    if (!indptrFilePath.empty() && !indptrReader) {
        std::lock_guard lock(parquetReaderMutex);
        if (!indptrReader) {
            std::vector<bool> columnSkips; // Read all columns
            auto context = transaction->getClientContext();
            indptrReader = std::make_unique<ParquetReader>(indptrFilePath, columnSkips, context);
        }
    }
}

void ParquetRelTable::loadNodeMappingData(Transaction* transaction) const {
    if (nodeMapping.empty() && !nodeMappingFilePath.empty()) {
        std::lock_guard lock(parquetReaderMutex);
        if (nodeMapping.empty()) {
            // Initialize node mapping reader if not already done
            if (!nodeMappingReader) {
                std::vector<bool> columnSkips; // Read all columns
                auto context = transaction->getClientContext();
                nodeMappingReader =
                    std::make_unique<ParquetReader>(nodeMappingFilePath, columnSkips, context);
            }

            // Initialize scan to populate column types
            auto context = transaction->getClientContext();
            auto vfs = VirtualFileSystem::GetUnsafe(*context);
            std::vector<uint64_t> groupsToRead;
            for (uint64_t i = 0; i < nodeMappingReader->getNumRowsGroups(); ++i) {
                groupsToRead.push_back(i);
            }

            ParquetReaderScanState scanState;
            nodeMappingReader->initializeScan(scanState, groupsToRead, vfs);

            // Check if the node mapping file has columns
            auto numColumns = nodeMappingReader->getNumColumns();
            if (numColumns < 2) {
                throw RuntimeException("Node mapping parquet file must have at least 2 columns");
            }

            // Validate column types for node mapping
            const auto& csrNodeIdType = nodeMappingReader->getColumnType(0);
            const auto& nodeTableIdType = nodeMappingReader->getColumnType(1);
            if (!LogicalTypeUtils::isIntegral(csrNodeIdType.getLogicalTypeID()) ||
                !LogicalTypeUtils::isIntegral(nodeTableIdType.getLogicalTypeID())) {
                throw RuntimeException(
                    "Node mapping parquet file columns must be integer types (columns 0 and 1)");
            }

            // Read the node mapping data
            DataChunk dataChunk(2);

            // Get column types
            for (uint32_t i = 0; i < 2 && i < numColumns; ++i) {
                const auto& columnTypeRef = nodeMappingReader->getColumnType(i);
                auto columnType = columnTypeRef.copy();
                auto vector = std::make_shared<ValueVector>(std::move(columnType));
                dataChunk.insert(i, vector);
            }

            // Read all node mapping values
            while (nodeMappingReader->scanInternal(scanState, dataChunk)) {
                auto selSize = dataChunk.state->getSelVector().getSelSize();
                for (size_t i = 0; i < selSize; ++i) {
                    auto csrNodeId = dataChunk.getValueVector(0).getValue<common::offset_t>(i);
                    auto nodeTableId = dataChunk.getValueVector(1).getValue<common::offset_t>(i);
                    nodeMapping[common::internalID_t(nodeTableId, getFromNodeTableID())] =
                        csrNodeId;
                    // Also create reverse mapping for destination node lookups
                    csrToNodeTableIdMap[csrNodeId] = nodeTableId;
                }
            }
        }
    }
}

void ParquetRelTable::loadIndptrData(Transaction* transaction) const {
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
            for (uint64_t i = 0; i < indptrReader->getNumRowsGroups(); ++i) {
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

bool ParquetRelTable::scanInternal(Transaction* transaction, TableScanState& scanState) {
    auto& relScanState = scanState.cast<RelTableScanState>();

    // Get the ParquetRelTableScanState
    auto& parquetRelScanState = static_cast<ParquetRelTableScanState&>(relScanState);

    // Readers are now initialized per scan state in initScanState
    // Load shared data (node mapping and indptr) - these are thread-safe to read
    loadNodeMappingData(transaction);
    if (!indptrFilePath.empty()) {
        loadIndptrData(transaction);
    }

    // True morsel-driven parallelism: each scan state processes its assigned row groups
    // Process all row groups assigned to this scan state, collecting relationships for bound nodes
    return scanInternalByRowGroups(transaction, parquetRelScanState);
}

bool ParquetRelTable::scanInternalByRowGroups(Transaction* transaction,
    ParquetRelTableScanState& parquetRelScanState) {
    // True morsel-driven parallelism: process assigned row groups and collect relationships for
    // bound nodes

    // Check if we have any row groups left to process
    if (parquetRelScanState.currentRowGroup >= parquetRelScanState.endRowGroup) {
        // No more row groups to process
        auto newSelVector = std::make_shared<SelectionVector>(0);
        parquetRelScanState.outState->setSelVector(newSelVector);
        return false;
    }

    // Process the current row group
    std::vector<uint64_t> rowGroupsToProcess = {parquetRelScanState.currentRowGroup};

    // Create a set of bound node IDs for fast lookup
    std::unordered_set<common::offset_t> boundNodeOffsets;
    for (size_t i = 0; i < parquetRelScanState.cachedBoundNodeSelVector.getSelSize(); ++i) {
        common::sel_t boundNodeIdx = parquetRelScanState.cachedBoundNodeSelVector[i];
        const auto boundNodeID = parquetRelScanState.nodeIDVector->getValue<nodeID_t>(boundNodeIdx);
        boundNodeOffsets.insert(boundNodeID.offset);
    }

    // Scan the current row group and collect relationships for bound nodes
    bool hasData = scanRowGroupForBoundNodes(transaction, parquetRelScanState, rowGroupsToProcess,
        boundNodeOffsets);

    // Move to next row group for next call
    parquetRelScanState.currentRowGroup++;

    return hasData;
}

common::offset_t ParquetRelTable::findSourceNodeForRow(common::offset_t globalRowIdx) const {
    // Binary search in indptrData to find which source node this row belongs to
    // indptrData[i] gives the starting row index for source node i
    // indptrData[i+1] gives the ending row index for source node i

    if (indptrData.empty()) {
        return common::INVALID_OFFSET;
    }

    // Binary search to find the source node
    size_t left = 0;
    size_t right = indptrData.size() - 2; // -2 because we compare with i+1

    while (left <= right) {
        size_t mid = left + (right - left) / 2;
        if (globalRowIdx >= indptrData[mid] && globalRowIdx < indptrData[mid + 1]) {
            return mid; // Found the source node
        } else if (globalRowIdx < indptrData[mid]) {
            if (mid == 0)
                break;
            right = mid - 1;
        } else {
            left = mid + 1;
        }
    }

    return common::INVALID_OFFSET; // Row not found in any range
}

bool ParquetRelTable::scanRowGroupForBoundNodes(Transaction* transaction,
    ParquetRelTableScanState& parquetRelScanState, const std::vector<uint64_t>& rowGroupsToProcess,
    const std::unordered_set<common::offset_t>& boundNodeOffsets) {

    // Initialize readers if needed
    initializeParquetReaders(transaction);

    if (!parquetRelScanState.indicesReader) {
        return false;
    }

    // Initialize scan state for the assigned row groups
    auto context = transaction->getClientContext();
    auto vfs = VirtualFileSystem::GetUnsafe(*context);
    parquetRelScanState.indicesReader->initializeScan(*parquetRelScanState.parquetScanState,
        rowGroupsToProcess, vfs);

    // Create DataChunk matching the indices parquet file schema
    auto numIndicesColumns = parquetRelScanState.indicesReader->getNumColumns();
    DataChunk indicesChunk(numIndicesColumns);

    // Insert value vectors for all columns in the parquet file
    for (uint32_t colIdx = 0; colIdx < numIndicesColumns; ++colIdx) {
        const auto& columnTypeRef = parquetRelScanState.indicesReader->getColumnType(colIdx);
        auto columnType = columnTypeRef.copy();
        auto vector = std::make_shared<ValueVector>(std::move(columnType));
        indicesChunk.insert(colIdx, vector);
    }

    // Scan the row groups and collect relationships for bound nodes
    uint64_t totalRowsCollected = 0;
    const uint64_t maxRowsPerCall = DEFAULT_VECTOR_CAPACITY;
    uint64_t currentGlobalRowIdx = 0;

    // Calculate the starting global row index for the first row group
    if (!rowGroupsToProcess.empty()) {
        auto metadata = parquetRelScanState.indicesReader->getMetadata();
        for (uint64_t rgIdx = 0; rgIdx < rowGroupsToProcess[0]; ++rgIdx) {
            currentGlobalRowIdx += metadata->row_groups[rgIdx].num_rows;
        }
    }

    while (totalRowsCollected < maxRowsPerCall &&
           parquetRelScanState.indicesReader->scanInternal(*parquetRelScanState.parquetScanState,
               indicesChunk)) {

        auto selSize = indicesChunk.state->getSelVector().getSelSize();

        for (size_t i = 0; i < selSize && totalRowsCollected < maxRowsPerCall;
             ++i, ++currentGlobalRowIdx) {
            // Find which source node this row belongs to
            common::offset_t sourceNodeOffset = findSourceNodeForRow(currentGlobalRowIdx);
            if (sourceNodeOffset == common::INVALID_OFFSET) {
                continue; // Invalid row
            }

            // Check if this source node is in our bound nodes
            if (boundNodeOffsets.find(sourceNodeOffset) == boundNodeOffsets.end()) {
                continue; // Not a bound node, skip
            }

            // This row belongs to a bound node, collect the relationship

            // Column 0 in indices file is the target/destination node ID
            // Read as offset_t and convert to INTERNAL_ID
            auto dstOffset = indicesChunk.getValueVector(0).getValue<common::offset_t>(i);
            auto dstNodeID = internalID_t(dstOffset, getToNodeTableID());

            // outputVectors[0] is the neighbor node ID (destination), if requested
            if (!parquetRelScanState.outputVectors.empty()) {
                parquetRelScanState.outputVectors[0]->setValue(totalRowsCollected, dstNodeID);
            }

            // If there are additional columns (e.g., weight), copy them to subsequent output
            // vectors These are property columns and should have matching types
            for (uint32_t colIdx = 1;
                 colIdx < numIndicesColumns && colIdx < parquetRelScanState.outputVectors.size();
                 ++colIdx) {
                parquetRelScanState.outputVectors[colIdx]->copyFromVectorData(totalRowsCollected,
                    &indicesChunk.getValueVector(colIdx), i);
            }

            totalRowsCollected++;
        }
    }

    // Set up the output state
    if (totalRowsCollected > 0) {
        auto selVector = std::make_shared<SelectionVector>(totalRowsCollected);
        selVector->setToFiltered(totalRowsCollected);
        for (uint64_t i = 0; i < totalRowsCollected; ++i) {
            (*selVector)[i] = i;
        }
        parquetRelScanState.outState->setSelVector(selVector);

        return true;
    } else {
        // No data found
        auto selVector = std::make_shared<SelectionVector>(0);
        parquetRelScanState.outState->setSelVector(selVector);
        return false;
    }
}

row_idx_t ParquetRelTable::getNumTotalRows(const transaction::Transaction* transaction) {
    initializeParquetReaders(const_cast<transaction::Transaction*>(transaction));
    if (!indicesReader) {
        return 0;
    }
    auto metadata = indicesReader->getMetadata();
    return metadata ? metadata->num_rows : 0;
}

} // namespace storage
} // namespace lbug
#include "storage/table/ice_disk_node_table.h"

#include <filesystem>
#include <mutex>

#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/data_chunk/sel_vector.h"
#include "common/exception/runtime.h"
#include "common/file_system/virtual_file_system.h"
#include "common/types/value/value.h"
#include "main/client_context.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/storage_manager.h"
#include "storage/storage_utils.h"
#include "storage/table/column.h"
#include "storage/table/ice_disk_utils.h"
#include "transaction/transaction.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::processor;
using namespace lbug::transaction;

namespace lbug {
namespace storage {

IceDiskNodeTable::IceDiskNodeTable(const StorageManager* storageManager,
    const NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager)
    : ColumnarNodeTableBase{storageManager, nodeTableEntry, memoryManager,
          std::make_unique<IceDiskNodeTableScanSharedState>()} {
    auto file = IceDiskUtils::constructNodeTablePath(
        IceDiskUtils::getBasePath(nodeTableEntry->getStorage()), nodeTableEntry->getName(),
        ".parquet");
    const auto dbDir =
        std::filesystem::path(storageManager->getDatabasePath()).parent_path().string();
    IceDiskUtils::checkVersionCompatibility(storageManager->getVFS(), dbDir, file);
    parquetFilePath = file;
}

void IceDiskNodeTable::initializeScanCoordination(const transaction::Transaction* transaction) {
    auto iceDiskScanSharedState =
        static_cast<IceDiskNodeTableScanSharedState*>(tableScanSharedState.get());
    auto numBatches = getNumBatches(transaction);
    iceDiskScanSharedState->reset(numBatches);
}

void IceDiskNodeTable::initScanState(Transaction* transaction, TableScanState& scanState,
    [[maybe_unused]] bool resetCachedBoundNodeSelVec) const {
    // Set up the scan state similar to how NodeTable does it
    auto& nodeScanState = scanState.cast<NodeTableScanState>();
    nodeScanState.source = TableScanSource::COMMITTED;

    // Note: Don't set nodeGroupIdx here - it's set by the morsel-driven parallelism system

    auto& iceDiskScanState = static_cast<IceDiskNodeTableScanState&>(nodeScanState);

    // Reset scan state for each scan to allow multiple scans of the same table in one query
    iceDiskScanState.dataRead = false;
    iceDiskScanState.allData.clear();
    iceDiskScanState.totalRows = 0;
    iceDiskScanState.nextRowToDistribute = 0;

    // Reset scan completion flag for this scan state
    iceDiskScanState.scanCompleted = false;

    // Each scan state gets its own parquet reader for thread safety
    if (!iceDiskScanState.initialized) {
        auto context = transaction->getClientContext();
        if (!context) {
            throw RuntimeException("Invalid client context for parquet scan state initialization");
        }

        std::vector<bool> columnSkips;
        try {
            auto resolvedPath = VirtualFileSystem::resolvePath(context, parquetFilePath);
            iceDiskScanState.parquetReader =
                std::make_unique<ParquetReader>(resolvedPath, columnSkips, context);
            iceDiskScanState.initialized = true;
        } catch (const std::exception& e) {
            throw RuntimeException("Failed to initialize parquet reader for file '" +
                                   parquetFilePath + "': " + e.what());
        }
    }

    // Set nodeGroupIdx to invalid initially - will be assigned by getNextBatch
    iceDiskScanState.nodeGroupIdx = INVALID_NODE_GROUP_IDX;

    // Initialize scan state for the current row group (assigned via shared state)
    initParquetScanForRowGroup(transaction, iceDiskScanState);
}

common::node_group_idx_t IceDiskNodeTable::getNumBatches(const Transaction* transaction) const {
    auto context = transaction->getClientContext();
    if (!context) {
        return 1;
    }

    std::vector<bool> columnSkips;
    try {
        auto resolvedPath = VirtualFileSystem::resolvePath(context, parquetFilePath);
        auto tempReader = std::make_unique<ParquetReader>(resolvedPath, columnSkips, context);
        return tempReader->getNumRowGroups();
    } catch (const std::exception& e) {
        return 1; // Fallback
    }
}

void IceDiskNodeTable::initParquetScanForRowGroup(Transaction* transaction,
    IceDiskNodeTableScanState& iceDiskScanState) const {
    auto context = transaction->getClientContext();
    if (!context) {
        return;
    }

    auto vfs = VirtualFileSystem::GetUnsafe(*context);
    if (!vfs) {
        return;
    }

    // Defensive check: ensure parquet reader exists
    if (!iceDiskScanState.parquetReader) {
        return;
    }

    // Defensive check: ensure parquet scan state exists
    if (!iceDiskScanState.parquetScanState) {
        return;
    }

    std::vector<uint64_t> groupsToRead;

    // Use shared state to get the next available row group for this scan state
    if (iceDiskScanState.nodeGroupIdx == INVALID_NODE_GROUP_IDX) {
        common::node_group_idx_t assignedRowGroup;
        if (dynamic_cast<IceDiskNodeTableScanSharedState*>(tableScanSharedState.get())
                ->getNextBatch(assignedRowGroup)) {
            iceDiskScanState.nodeGroupIdx = assignedRowGroup;
            groupsToRead.push_back(assignedRowGroup);
        } else {
            // No more row groups available - mark scan as completed
            iceDiskScanState.scanCompleted = true;
            // Still need to initialize the scan state with empty groups so reader is in valid state
            iceDiskScanState.parquetReader->initializeScan(*iceDiskScanState.parquetScanState,
                groupsToRead, vfs);
            return;
        }
    } else {
        // Row group already assigned (e.g., by external morsel system or re-initialization)
        groupsToRead.push_back(iceDiskScanState.nodeGroupIdx);
    }

    // Re-initialize scan for the specific row groups
    // Note: initializeScan can be called multiple times; the first call populates column metadata
    iceDiskScanState.parquetReader->initializeScan(*iceDiskScanState.parquetScanState, groupsToRead,
        vfs);
}

bool IceDiskNodeTable::scanInternal(Transaction* transaction, TableScanState& scanState) {
    auto& iceDiskScanState = static_cast<IceDiskNodeTableScanState&>(scanState);

    // Check if this particular scan state has already completed
    if (iceDiskScanState.scanCompleted) {
        return false;
    }

    scanState.resetOutVectors();

    // Read all data once into scan state
    if (!iceDiskScanState.dataRead) {
        // Only the first thread reads the parquet data
        if (!iceDiskScanState.initialized) {
            return false;
        }

        // Create a data chunk for reading parquet data
        auto numColumns = iceDiskScanState.parquetReader->getNumColumns();

        // Defensive check: ensure parquet file has at least one column
        if (numColumns == 0) {
            throw RuntimeException("Parquet file '" + parquetFilePath + "' has no columns");
        }

        DataChunk parquetDataChunk(numColumns, scanState.outState);

        // Create vectors with parquet types
        // Defensive check: ensure parquet file has enough columns for what we expect
        // Always create the data chunk to match the exact number of parquet columns
        // to prevent crashes in the parquet reader when accessing result vectors
        for (uint32_t i = 0; i < numColumns; ++i) {
            const auto& parquetColumnType = iceDiskScanState.parquetReader->getColumnType(i);
            auto columnType = parquetColumnType.copy();
            auto vector = std::make_shared<ValueVector>(std::move(columnType),
                MemoryManager::Get(*transaction->getClientContext()), scanState.outState);
            parquetDataChunk.insert(i, vector);
        }

        // Read from parquet
        iceDiskScanState.parquetReader->scan(*iceDiskScanState.parquetScanState, parquetDataChunk);

        auto selSize = parquetDataChunk.state->getSelVector().getSelSize();
        if (selSize > 0) {
            iceDiskScanState.allData.resize(selSize);
            for (size_t row = 0; row < selSize; ++row) {
                iceDiskScanState.allData[row].resize(
                    scanState.outputVectors
                        .size()); // Use output vector count, not parquet column count

                // Map parquet columns to correct output vector positions by name
                // Defensive check: ensure we don't access more columns than available in the chunk
                auto maxParquetCol = std::min(static_cast<size_t>(numColumns),
                    static_cast<size_t>(parquetDataChunk.getNumValueVectors()));

                for (size_t parquetCol = 0; parquetCol < maxParquetCol; ++parquetCol) {
                    // Defensive check: ensure the column index is valid for the data chunk
                    if (parquetCol >= parquetDataChunk.getNumValueVectors()) {
                        continue;
                    }

                    auto& srcVector = parquetDataChunk.getValueVectorMutable(parquetCol);

                    // Get parquet column name and find its corresponding column ID
                    std::string parquetColumnName =
                        iceDiskScanState.parquetReader->getColumnName(parquetCol);
                    auto nodeTableEntry = this->nodeTableCatalogEntry;

                    // Check if the column exists first before calling getColumnID
                    if (!nodeTableEntry->containsProperty(parquetColumnName)) {
                        // Column doesn't exist in table schema, skip it
                        continue;
                    }

                    // Find the column ID for this property name
                    column_id_t parquetColumnID = nodeTableEntry->getColumnID(parquetColumnName);

                    // Find which output vector position corresponds to this column ID
                    size_t outputCol = INVALID_COLUMN_ID;
                    for (size_t outCol = 0; outCol < scanState.columnIDs.size(); ++outCol) {
                        if (scanState.columnIDs[outCol] == parquetColumnID) {
                            outputCol = outCol;
                            break;
                        }
                    }

                    // Only copy data if we found a matching output position
                    if (outputCol != INVALID_COLUMN_ID &&
                        outputCol < iceDiskScanState.allData[row].size()) {
                        // Defensive check: ensure the row index is valid for the source vector
                        if (row >= srcVector.state->getSelVector().getSelSize()) {
                            continue;
                        }

                        if (srcVector.isNull(row)) {
                            iceDiskScanState.allData[row][outputCol] =
                                std::make_unique<Value>(Value::createNullValue());
                        } else {
                            iceDiskScanState.allData[row][outputCol] =
                                std::make_unique<Value>(*srcVector.getAsValue(row));
                        }
                    }
                }
            }
            iceDiskScanState.totalRows = selSize;
        }
        iceDiskScanState.dataRead = true;
    }

    // Now distribute one row to this scan state
    if (iceDiskScanState.nextRowToDistribute >= iceDiskScanState.totalRows) {
        iceDiskScanState.scanCompleted = true;
        return false; // No more rows to distribute
    }

    size_t rowIndex = iceDiskScanState.nextRowToDistribute++;

    // Copy one row to output vectors
    // Defensive checks: ensure valid row index and handle empty data gracefully
    if (rowIndex >= iceDiskScanState.allData.size()) {
        iceDiskScanState.scanCompleted = true;
        return false;
    }

    auto numColumns =
        std::min(scanState.outputVectors.size(), iceDiskScanState.allData[rowIndex].size());
    for (size_t col = 0; col < numColumns; ++col) {
        // Defensive check: ensure output vector exists
        if (col >= scanState.outputVectors.size() || !scanState.outputVectors[col]) {
            continue;
        }

        auto& dstVector = *scanState.outputVectors[col];

        // Defensive check: ensure value exists for this column
        if (col >= iceDiskScanState.allData[rowIndex].size() ||
            !iceDiskScanState.allData[rowIndex][col]) {
            dstVector.setNull(0, true);
            continue;
        }

        auto& value = *iceDiskScanState.allData[rowIndex][col];

        if (value.isNull()) {
            dstVector.setNull(0, true);
        } else {
            dstVector.copyFromValue(0, value);
        }
    }

    // Set node ID for this row
    auto tableID = this->getTableID();
    auto& nodeID = scanState.nodeIDVector->getValue<nodeID_t>(0);
    nodeID.tableID = tableID;
    nodeID.offset = rowIndex; // Use the actual row index from parquet

    scanState.outState->getSelVectorUnsafe().setSelSize(1); // Return exactly one row
    return true;
}

row_idx_t IceDiskNodeTable::getTotalRowCount(const Transaction* transaction) const {
    const auto cached = cachedRowCount.load(std::memory_order_relaxed);
    if (cached != INVALID_ROW_IDX) {
        return cached;
    }
    // Create a temporary reader to get metadata
    auto context = transaction->getClientContext();
    if (!context) {
        return 0;
    }

    std::vector<bool> columnSkips;

    try {
        auto resolvedPath = VirtualFileSystem::resolvePath(context, parquetFilePath);
        auto tempReader = std::make_unique<ParquetReader>(resolvedPath, columnSkips, context);
        if (!tempReader) {
            return 0;
        }
        auto metadata = tempReader->getMetadata();
        const auto count = metadata ? metadata->num_rows : 0;
        cachedRowCount.store(static_cast<row_idx_t>(count), std::memory_order_relaxed);
        return count;
    } catch (const std::exception& e) {
        // If parquet file is corrupted or invalid, return 0 instead of crashing
        return 0;
    }
}

bool IceDiskNodeTable::isVisible(const transaction::Transaction* transaction,
    common::offset_t offset) const {
    return offset < getTotalRowCount(transaction);
}

bool IceDiskNodeTable::isVisibleNoLock(const transaction::Transaction* transaction,
    common::offset_t offset) const {
    return offset < getTotalRowCount(transaction);
}

} // namespace storage
} // namespace lbug

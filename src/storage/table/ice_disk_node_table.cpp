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
#include "transaction/transaction.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::processor;
using namespace lbug::transaction;

namespace lbug {
namespace storage {

IceDiskNodeTable::IceDiskNodeTable(const StorageManager* storageManager,
    const NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager)
    : NodeTable{storageManager, nodeTableEntry, memoryManager},
      nodeTableCatalogEntry{nodeTableEntry},
      tableScanSharedState{std::make_unique<IceDiskNodeTableScanSharedState>()} {
    if (nodeTableEntry->getTablePath().empty()) {
        throw RuntimeException("Parquet file path is empty for icebug-disk-backed node table");
    }

    parquetFilePath = nodeTableEntry->getTablePath();
}

void IceDiskNodeTable::initializeScanCoordination(const Transaction* transaction) {
    auto context = transaction->getClientContext();
    if (context) {
        auto resolvedPath = VirtualFileSystem::resolvePath(context, parquetFilePath);
        auto tempReader = std::make_unique<ParquetReader>(resolvedPath, std::vector<bool>(), context);
        auto metadata = tempReader->getMetadata();
        uint64_t currentStartOffset = 0;

        rowGroupStartOffsets.clear();
        for (std::size_t i = 0; i < metadata->row_groups.size(); ++i) {
            rowGroupStartOffsets.push_back(currentStartOffset);
            currentStartOffset += metadata->row_groups[i].num_rows;
        }

        tableScanSharedState->reset(tempReader->getNumRowGroups());
    }
}

void IceDiskNodeTable::initScanState(Transaction* transaction, TableScanState& scanState,
    bool /*resetCachedBoundNodeSelVec*/) const {
    auto& iceDiskNodeScanState = static_cast<IceDiskNodeTableScanState&>(scanState);

    if(iceDiskNodeScanState.currentRowGroupIdx != static_cast<std::size_t>(common::INVALID_NODE_GROUP_IDX)) {
        iceDiskNodeScanState.scanCompleted = true;
        return;
    }

    iceDiskNodeScanState.scanCompleted = false;
    iceDiskNodeScanState.dataReadCompleted = false;
    iceDiskNodeScanState.data.clear();
    iceDiskNodeScanState.currentRowGroupBatchOffset = 0;

    // Each scan state gets its own parquet reader for thread safety and initialized only once
    if (!iceDiskNodeScanState.initialized) {
        auto context = transaction->getClientContext();
        if (!context) {
            throw RuntimeException("Invalid client context for IceDisk scan state initialization");
        }

        try {
            auto resolvedPath = VirtualFileSystem::resolvePath(context, parquetFilePath);
            iceDiskNodeScanState.parquetReader =
                std::make_unique<ParquetReader>(resolvedPath, std::vector<bool>(), context);
            iceDiskNodeScanState.initialized = true;
        } catch (const std::exception& e) {
            throw RuntimeException("Failed to initialize parquet reader for file '" +
                                   parquetFilePath + "': " + e.what());
        }
    }

    // Initialize scan state for the current row group (assigned via shared state)
    initIceDiskScanForRowGroup(transaction, iceDiskNodeScanState);
}

void IceDiskNodeTable::initIceDiskScanForRowGroup(Transaction* transaction,
    IceDiskNodeTableScanState& scanState) const {
    auto context = transaction->getClientContext();
    if (!context) {
        return;
    }

    auto vfs = VirtualFileSystem::GetUnsafe(*context);
    if (!vfs) {
        return;
    }

    // Defensive check: ensure parquet reader exists
    if (!scanState.parquetReader) {
        return;
    }

    // Defensive check: ensure parquet scan state exists
    if (!scanState.parquetScanState) {
        return;
    }

    // Re-initialize scan for the specific row groups
    // Note: initializeScan can be called multiple times; the first call populates column metadata
    scanState.parquetReader->initializeScan(*scanState.parquetScanState, {scanState.currentRowGroupIdx}, vfs);
}


// First run always fails due to iceDiskNodeScanState.scanCompleted == true as
// scanState.currentRowGroupIdx = INVALID_NODE_GROUP_IDX on the first
// run(look at initScanState function) tableScanSharedState.nextMorsel will drive the morsel assignment
bool IceDiskNodeTable::scanInternal(Transaction* transaction, TableScanState& scanState) {
    auto& iceDiskNodeScanState = static_cast<IceDiskNodeTableScanState&>(scanState);
    if (iceDiskNodeScanState.scanCompleted) {
        return false;
    }

    scanState.resetOutVectors();

    // Read all data once into scan state
    if (!iceDiskNodeScanState.dataReadCompleted) {
        readParquetData(transaction, scanState);
    }

    if (iceDiskNodeScanState.currentRowGroupBatchOffset >= iceDiskNodeScanState.data.size()) {
        iceDiskNodeScanState.scanCompleted = true;
        return false;
    }

    auto outputSize = std::min(scanRowGroupBatchSize, iceDiskNodeScanState.data.size() - iceDiskNodeScanState.currentRowGroupBatchOffset);
    auto numColumns =
        std::min(scanState.outputVectors.size(), iceDiskNodeScanState.data[iceDiskNodeScanState.currentRowGroupBatchOffset].size());

    for (std::size_t col = 0; col < numColumns; ++col) {
        auto& dstVector = *scanState.outputVectors[col];

        for (std::size_t i = 0; i < outputSize; ++i) {
            auto& value = *iceDiskNodeScanState.data[iceDiskNodeScanState.currentRowGroupBatchOffset + i][col];
            if (value.isNull()) {
                dstVector.setNull(i, true);
            } else {
                dstVector.copyFromValue(i, value);
            }
        }
    }

    for (std::size_t i = 0; i < outputSize; ++i) {
        auto& nodeID = scanState.nodeIDVector->getValue<common::nodeID_t>(i);
        nodeID.tableID = tableID;
        // assign parquet rowIndex
        nodeID.offset = rowGroupStartOffsets[iceDiskNodeScanState.currentRowGroupIdx] + iceDiskNodeScanState.currentRowGroupBatchOffset + i;
    }

    iceDiskNodeScanState.currentRowGroupBatchOffset += outputSize;
    scanState.outState->getSelVectorUnsafe().setSelSize(outputSize);
    return true;
}

void IceDiskNodeTable::readParquetData(Transaction* transaction, TableScanState& scanState) const {
    auto& iceDiskNodeScanState = static_cast<IceDiskNodeTableScanState&>(scanState);
    auto numColumns = iceDiskNodeScanState.parquetReader->getNumColumns();

    // Defensive check: ensure parquet file has at least one column
    if (numColumns == 0) {
        throw RuntimeException("Parquet file '" + parquetFilePath + "' has no columns");
    }

    // Create vectors with parquet types
    // Always create the data chunk to match the exact number of parquet columns
    // to prevent crashes in the parquet reader when accessing result vectors
    DataChunk parquetDataChunk(numColumns, scanState.outState);

    for (uint32_t i = 0; i < numColumns; ++i) {
        const auto& parquetColumnType = iceDiskNodeScanState.parquetReader->getColumnType(i);
        auto columnType = parquetColumnType.copy();
        auto vector = std::make_shared<ValueVector>(std::move(columnType),
            MemoryManager::Get(*transaction->getClientContext()), scanState.outState);
        parquetDataChunk.insert(i, vector);
    }

    // Read from parquet
    iceDiskNodeScanState.parquetReader->scan(*iceDiskNodeScanState.parquetScanState, parquetDataChunk);

    auto selSize = parquetDataChunk.state->getSelVector().getSelSize();
    if (selSize > 0) {
        iceDiskNodeScanState.data.resize(selSize);
        for (std::size_t row = 0; row < selSize; ++row) {
            iceDiskNodeScanState.data[row].resize(
                scanState.outputVectors
                    .size()); // Use output vector count, not parquet column count

            // Map parquet columns to correct output vector positions by name
            // Defensive check: ensure we don't access more columns than available in the chunk
            auto maxParquetCol = std::min(static_cast<std::size_t>(numColumns),
                static_cast<std::size_t>(parquetDataChunk.getNumValueVectors()));

            for (std::size_t parquetCol = 0; parquetCol < maxParquetCol; ++parquetCol) {
                // Defensive check: ensure the column index is valid for the data chunk
                if (parquetCol >= parquetDataChunk.getNumValueVectors()) {
                    continue;
                }

                auto& srcVector = parquetDataChunk.getValueVectorMutable(parquetCol);

                // Get parquet column name and find its corresponding column ID
                std::string parquetColumnName =
                    iceDiskNodeScanState.parquetReader->getColumnName(parquetCol);

                // Check if the column exists first before calling getColumnID
                if (!nodeTableCatalogEntry->containsProperty(parquetColumnName)) {
                    // Column doesn't exist in table schema, skip it
                    continue;
                }

                // Find the column ID for this property name
                column_id_t parquetColumnID = nodeTableCatalogEntry->getColumnID(parquetColumnName);

                // Find which output vector position corresponds to this column ID
                std::size_t outputCol = INVALID_COLUMN_ID;
                for (std::size_t outCol = 0; outCol < scanState.columnIDs.size(); ++outCol) {
                    if (scanState.columnIDs[outCol] == parquetColumnID) {
                        outputCol = outCol;
                        break;
                    }
                }

                // Only copy data if we found a matching output position
                if (outputCol != INVALID_COLUMN_ID &&
                    outputCol < iceDiskNodeScanState.data[row].size()) {
                    // Defensive check: ensure the row index is valid for the source vector
                    if (row >= srcVector.state->getSelVector().getSelSize()) {
                        continue;
                    }

                    if (srcVector.isNull(row)) {
                        iceDiskNodeScanState.data[row][outputCol] =
                            std::make_unique<Value>(Value::createNullValue());
                    } else {
                        iceDiskNodeScanState.data[row][outputCol] =
                            std::make_unique<Value>(*srcVector.getAsValue(row));
                    }
                }
            }
        }
    }

    iceDiskNodeScanState.dataReadCompleted = true;
}

std::size_t IceDiskNodeTable::getNumTotalRows(const Transaction* transaction) {
    auto context = transaction->getClientContext();

    if (!context) {
        return 0;
    }

    try {
        auto resolvedPath = VirtualFileSystem::resolvePath(context, parquetFilePath);
        auto tempReader = std::make_unique<ParquetReader>(resolvedPath, std::vector<bool>(), context);

        return tempReader->getMetadata()->num_rows;
    } catch (const std::exception& e) {
        // If parquet file is corrupted or invalid, return 0 instead of crashing
        return 0;
    }
}

std::size_t IceDiskNodeTable::getNumRowGroups(const transaction::Transaction* transaction) const {
    auto context = transaction->getClientContext();

    if (!context) {
        return 0;
    }

    try {
        auto resolvedPath = VirtualFileSystem::resolvePath(context, parquetFilePath);
        auto tempReader = std::make_unique<ParquetReader>(resolvedPath, std::vector<bool>(), context);

        return tempReader->getNumRowGroups();
    } catch (const std::exception& e) {
        // If parquet file is corrupted or invalid, return 0 instead of crashing
        return 0;
    }
}

std::size_t IceDiskNodeTable::getNumScanMorsels(const transaction::Transaction* transaction) const {
    return getNumRowGroups(transaction);
}

} // namespace storage
} // namespace lbug

#include "storage/table/ice_disk_node_table.h"

#include <filesystem>
#include <mutex>

#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/data_chunk/sel_vector.h"
#include "common/exception/runtime.h"
#include "common/file_system/virtual_file_system.h"
#include "common/string_utils.h"
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
    const NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager,
    main::ClientContext* context)
    : ColumnarNodeTableBase{storageManager, nodeTableEntry, memoryManager,
          std::make_unique<IceDiskNodeTableScanSharedState>()} {
    const auto& storage = nodeTableEntry->getStorage();
    auto path =
        common::StringUtils::getLower(storage).ends_with("parquet") ?
            storage :
            IceDiskUtils::constructNodeTablePath(storage, nodeTableEntry->getName(), ".parquet");
    auto resolvedPath = common::VirtualFileSystem::resolvePath(context, path);
    IceDiskUtils::checkVersionCompatibility(context, resolvedPath);
    parquetFilePath = resolvedPath;
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
            iceDiskScanState.parquetReader =
                std::make_unique<ParquetReader>(parquetFilePath, columnSkips, context);
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
        auto tempReader = std::make_unique<ParquetReader>(parquetFilePath, columnSkips, context);
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

    if (!iceDiskScanState.initialized) {
        return false;
    }

    auto numColumns = iceDiskScanState.parquetReader->getNumColumns();
    if (numColumns == 0) {
        throw RuntimeException("Parquet file '" + parquetFilePath + "' has no columns");
    }

    DataChunk parquetDataChunk(numColumns, scanState.outState);
    for (uint32_t i = 0; i < numColumns; ++i) {
        const auto& parquetColumnType = iceDiskScanState.parquetReader->getColumnType(i);
        auto columnType = parquetColumnType.copy();
        auto vector = std::make_shared<ValueVector>(std::move(columnType),
            MemoryManager::Get(*transaction->getClientContext()), scanState.outState);
        parquetDataChunk.insert(i, vector);
    }

    parquetDataChunk.state->getSelVectorUnsafe().setToFiltered(0);
    iceDiskScanState.parquetReader->scan(*iceDiskScanState.parquetScanState, parquetDataChunk);
    auto selSize = parquetDataChunk.state->getSelVector().getSelSize();
    if (selSize == 0) {
        iceDiskScanState.scanCompleted = true;
        return false;
    }

    auto metadata = iceDiskScanState.parquetReader->getMetadata();
    offset_t startOffset = 0;
    auto currentRowGroupIdx = iceDiskScanState.nodeGroupIdx;
    if (iceDiskScanState.parquetScanState->currentGroup >= 0 &&
        static_cast<uint64_t>(iceDiskScanState.parquetScanState->currentGroup) <
            iceDiskScanState.parquetScanState->groupIdxList.size()) {
        currentRowGroupIdx = static_cast<common::node_group_idx_t>(iceDiskScanState
                .parquetScanState->groupIdxList[iceDiskScanState.parquetScanState->currentGroup]);
    }
    for (common::node_group_idx_t rg = 0;
        rg < currentRowGroupIdx && rg < metadata->row_groups.size(); ++rg) {
        startOffset += metadata->row_groups[rg].num_rows;
    }
    startOffset += iceDiskScanState.parquetScanState->groupOffset - selSize;

    std::vector<size_t> outputToParquetColumn(scanState.outputVectors.size(), INVALID_COLUMN_ID);
    for (size_t parquetCol = 0; parquetCol < numColumns; ++parquetCol) {
        auto parquetColumnName = iceDiskScanState.parquetReader->getColumnName(parquetCol);
        if (!nodeTableCatalogEntry->containsProperty(parquetColumnName)) {
            continue;
        }
        auto parquetColumnID = nodeTableCatalogEntry->getColumnID(parquetColumnName);
        for (size_t outCol = 0; outCol < scanState.columnIDs.size(); ++outCol) {
            if (scanState.columnIDs[outCol] == parquetColumnID &&
                outCol < outputToParquetColumn.size()) {
                outputToParquetColumn[outCol] = parquetCol;
                break;
            }
        }
    }

    for (size_t outCol = 0; outCol < scanState.outputVectors.size(); ++outCol) {
        auto* dstVector = scanState.outputVectors[outCol];
        if (!dstVector) {
            continue;
        }
        auto parquetCol = outputToParquetColumn[outCol];
        if (parquetCol == INVALID_COLUMN_ID ||
            parquetCol >= parquetDataChunk.getNumValueVectors()) {
            for (size_t row = 0; row < selSize; ++row) {
                dstVector->setNull(row, true);
            }
            continue;
        }
        auto& srcVector = parquetDataChunk.getValueVector(parquetCol);
        for (size_t row = 0; row < selSize; ++row) {
            dstVector->copyFromVectorData(row, &srcVector, row);
        }
    }

    auto tableID = this->getTableID();
    for (size_t row = 0; row < selSize; ++row) {
        auto& nodeID = scanState.nodeIDVector->getValue<nodeID_t>(row);
        nodeID.tableID = tableID;
        nodeID.offset = startOffset + row;
    }

    scanState.outState->getSelVectorUnsafe().setToUnfiltered(selSize);
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
        auto tempReader = std::make_unique<ParquetReader>(parquetFilePath, columnSkips, context);
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

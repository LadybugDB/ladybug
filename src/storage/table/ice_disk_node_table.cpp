#include "storage/table/ice_disk_node_table.h"

#include <filesystem>
#include <mutex>

#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/constants.h"
#include "common/data_chunk/sel_vector.h"
#include "common/exception/runtime.h"
#include "common/file_system/virtual_file_system.h"
#include "common/types/value/value.h"
#include "main/client_context.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/storage_manager.h"
#include "storage/table/ice_disk_utils.h"
#include "transaction/transaction.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::processor;
using namespace lbug::transaction;

namespace lbug {
namespace storage {

void IceDiskNodeTableScanState::setToTable(const transaction::Transaction* /*transaction*/,
    Table* table_, std::vector<common::column_id_t> columnIDs_,
    std::vector<ColumnPredicateSet> columnPredicateSets_, common::RelDataDirection /*direction*/) {
    // TableScanState::setToTable(transaction, table_, columnIDs_, std::move(columnPredicateSets_));
    table = table_;
    columnIDs = std::move(columnIDs_);
    columnPredicateSets = std::move(columnPredicateSets_);
    // IceDisk node tables don't use NodeGroup infrastructure; skip the base class
    // which would dereference the uninitialized nodeGroupScanState.
}

IceDiskNodeTable::IceDiskNodeTable(const StorageManager* storageManager,
    const NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager)
    : NodeTable{storageManager, nodeTableEntry, memoryManager},
      parquetFilePath{IceDiskUtils::constructNodeTablePath(
          IceDiskUtils::getBasePath(nodeTableEntry->getStorage()), nodeTableEntry->getName(),
          ".parquet")},
      nodeTableCatalogEntry{nodeTableEntry},
      tableScanSharedState{std::make_unique<IceDiskNodeTableScanSharedState>()} {}

void IceDiskNodeTable::initializeScanCoordination(const Transaction* transaction) {
    auto context = transaction->getClientContext();
    if (context) {
        auto resolvedPath = VirtualFileSystem::resolvePath(context, parquetFilePath);
        auto tempReader =
            std::make_unique<ParquetReader>(resolvedPath, std::vector<bool>(), context);
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

    if (iceDiskNodeScanState.currentRowGroupIdx ==
        static_cast<std::size_t>(common::INVALID_NODE_GROUP_IDX)) {
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
    scanState.parquetReader->initializeScan(*scanState.parquetScanState,
        {scanState.currentRowGroupIdx}, vfs);
}

bool IceDiskNodeTable::scanInternal(Transaction* transaction, TableScanState& scanState) {
    auto& iceDiskNodeScanState = static_cast<IceDiskNodeTableScanState&>(scanState);
    if (iceDiskNodeScanState.scanCompleted) {
        return false;
    }

    scanState.resetOutVectors();

    // Read data for the current row group if not yet done
    if (!iceDiskNodeScanState.dataReadCompleted) {
        readParquetData(transaction, scanState);
    }

    if (iceDiskNodeScanState.currentRowGroupBatchOffset >= iceDiskNodeScanState.data.size()) {
        iceDiskNodeScanState.scanCompleted = true;
        return false;
    }

    auto outputSize = std::min(scanRowGroupBatchSize,
        iceDiskNodeScanState.data.size() - iceDiskNodeScanState.currentRowGroupBatchOffset);
    auto numColumns = std::min(scanState.outputVectors.size(),
        iceDiskNodeScanState.data[iceDiskNodeScanState.currentRowGroupBatchOffset].size());

    for (std::size_t col = 0; col < numColumns; ++col) {
        auto& dstVector = *scanState.outputVectors[col];

        for (std::size_t i = 0; i < outputSize; ++i) {
            auto& value = *iceDiskNodeScanState
                               .data[iceDiskNodeScanState.currentRowGroupBatchOffset + i][col];
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
        nodeID.offset = rowGroupStartOffsets[iceDiskNodeScanState.currentRowGroupIdx] +
                        iceDiskNodeScanState.currentRowGroupBatchOffset + i;
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

    // Fresh DataChunk with its own state — do NOT share scanState.outState; we accumulate
    // rows across batches while the output state is managed by scanInternal() above.
    DataChunk parquetDataChunk(numColumns);
    auto* memMgr = MemoryManager::Get(*transaction->getClientContext());
    for (uint32_t i = 0; i < numColumns; ++i) {
        auto columnType = iceDiskNodeScanState.parquetReader->getColumnType(i).copy();
        parquetDataChunk.insert(i, std::make_shared<ValueVector>(std::move(columnType), memMgr));
    }

    // Pre-compute parquet-column → output-column mapping once.
    const auto numCols = static_cast<std::size_t>(parquetDataChunk.getNumValueVectors());
    std::vector<std::size_t> colMap(numCols, INVALID_COLUMN_ID);
    for (std::size_t pc = 0; pc < numCols; ++pc) {
        const auto& name = iceDiskNodeScanState.parquetReader->getColumnName(pc);
        if (!nodeTableCatalogEntry->containsProperty(name)) {
            continue;
        }
        const auto colID = nodeTableCatalogEntry->getColumnID(name);
        for (std::size_t oc = 0; oc < scanState.columnIDs.size(); ++oc) {
            if (scanState.columnIDs[oc] == colID) {
                colMap[pc] = oc;
                break;
            }
        }
    }

    // scanInternal() returns true on the initial row-group setup call (batchSize == 0) and on
    // each data batch; returns false when the row group is exhausted. Loop to read ALL rows.
    while (iceDiskNodeScanState.parquetReader->scanInternal(
        *iceDiskNodeScanState.parquetScanState, parquetDataChunk)) {
        const auto batchSize = parquetDataChunk.state->getSelVector().getSelSize();
        if (batchSize == 0) {
            continue; // row-group setup call — no data yet
        }

        const auto base = iceDiskNodeScanState.data.size();
        iceDiskNodeScanState.data.resize(base + batchSize);

        for (std::size_t row = 0; row < batchSize; ++row) {
            iceDiskNodeScanState.data[base + row].resize(scanState.outputVectors.size());
            for (std::size_t pc = 0; pc < numCols; ++pc) {
                const auto oc = colMap[pc];
                if (oc == INVALID_COLUMN_ID || oc >= iceDiskNodeScanState.data[base + row].size()) {
                    continue;
                }
                auto& srcVector = parquetDataChunk.getValueVectorMutable(pc);
                if (srcVector.isNull(row)) {
                    iceDiskNodeScanState.data[base + row][oc] =
                        std::make_unique<Value>(Value::createNullValue());
                } else {
                    iceDiskNodeScanState.data[base + row][oc] =
                        std::make_unique<Value>(*srcVector.getAsValue(row));
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
        auto tempReader =
            std::make_unique<ParquetReader>(resolvedPath, std::vector<bool>(), context);

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
        auto tempReader =
            std::make_unique<ParquetReader>(resolvedPath, std::vector<bool>(), context);

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

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
    auto iceDiskScanSharedState =
        static_cast<IceDiskNodeTableScanSharedState*>(tableScanSharedState.get());
    auto numRowGroups = getNumRowGroups(transaction);
    iceDiskScanSharedState->reset(numRowGroups);
}

void IceDiskNodeTable::initScanState(Transaction* /*transaction*/, TableScanState& scanState,
    bool /*resetCachedBoundNodeSelVec*/) const {
    auto& iceDiskNodeScanState = static_cast<IceDiskNodeTableScanState&>(scanState);
    iceDiskNodeScanState.source = TableScanSource::COMMITTED;
    iceDiskNodeScanState.scanCompleted = false;
    iceDiskNodeScanState.nodeGroupIdx = INVALID_NODE_GROUP_IDX;
    iceDiskNodeScanState.currentRowOffset = 0;
}

bool IceDiskNodeTable::scanInternal(Transaction* transaction, TableScanState& scanState) {
    auto& iceDiskNodeScanState = static_cast<IceDiskNodeTableScanState&>(scanState);
    if (iceDiskNodeScanState.scanCompleted) {
        return false;
    }

    auto vfs = VirtualFileSystem::GetUnsafe(*transaction->getClientContext());
    DataChunk dataChunk(iceDiskNodeScanState.parquetReader->getNumColumns());
    for (uint32_t i = 0; i < iceDiskNodeScanState.parquetReader->getNumColumns(); ++i) {
        dataChunk.insert(i, std::make_shared<ValueVector>(
                               iceDiskNodeScanState.parquetReader->getColumnType(i).copy(),
                               MemoryManager::Get(*transaction->getClientContext())));
    }

    while (true) {
        if (iceDiskNodeScanState.nodeGroupIdx == INVALID_NODE_GROUP_IDX) {
            if (!tableScanSharedState->getNextMorsel(&iceDiskNodeScanState)) {
                iceDiskNodeScanState.scanCompleted = true;
                return false;
            }
            iceDiskNodeScanState.currentRowOffset = 0;
            std::vector<uint64_t> groupsToRead = {iceDiskNodeScanState.nodeGroupIdx};
            iceDiskNodeScanState.parquetReader->initializeScan(*iceDiskNodeScanState.parquetScanState,
                groupsToRead, vfs);
        }

        dataChunk.state->getSelVectorUnsafe().setSelSize(0);
        iceDiskNodeScanState.parquetReader->scan(*iceDiskNodeScanState.parquetScanState, dataChunk);
        if (dataChunk.state->getSelVector().getSelSize() == 0) {
            iceDiskNodeScanState.nodeGroupIdx = INVALID_NODE_GROUP_IDX;
            iceDiskNodeScanState.currentRowOffset = 0;
            continue;
        }

        scanState.resetOutVectors();
        auto selSize = dataChunk.state->getSelVector().getSelSize();
        for (uint32_t i = 0; i < iceDiskNodeScanState.columnIDs.size(); ++i) {
            auto columnID = iceDiskNodeScanState.columnIDs[i];
            if (columnID == ROW_IDX_COLUMN_ID) {
                for (size_t j = 0; j < selSize; ++j) {
                    ((row_idx_t*)iceDiskNodeScanState.outputVectors[i]->getData())[j] =
                        iceDiskNodeScanState.currentStartRow + iceDiskNodeScanState.currentRowOffset + j;
                }
            } else if (columnID != INVALID_COLUMN_ID) {
                uint32_t parquetColIdx = 0;
                auto propertyName = nodeTableCatalogEntry->getProperty(columnID).getName();
                for (uint32_t j = 0; j < iceDiskNodeScanState.parquetReader->getNumColumns(); ++j) {
                    if (iceDiskNodeScanState.parquetReader->getColumnName(j) == propertyName) {
                        parquetColIdx = j;
                        break;
                    }
                }
                auto& srcVector = dataChunk.getValueVectorMutable(parquetColIdx);
                auto& dstVector = *iceDiskNodeScanState.outputVectors[i];
                for (size_t j = 0; j < selSize; ++j) {
                    dstVector.copyFromVectorData(j, &srcVector, dataChunk.state->getSelVector()[j]);
                }
            }
        }

        for (size_t i = 0; i < selSize; ++i) {
            ((nodeID_t*)iceDiskNodeScanState.nodeIDVector->getData())[i] = nodeID_t{
                iceDiskNodeScanState.currentStartRow + iceDiskNodeScanState.currentRowOffset + i,
                nodeTableCatalogEntry->getTableID()};
        }
        iceDiskNodeScanState.currentRowOffset += selSize;
        iceDiskNodeScanState.outState->getSelVectorUnsafe().setSelSize(selSize);
        iceDiskNodeScanState.outState->getSelVectorUnsafe().setToUnfiltered();
        return selSize > 0;
    }
}

common::row_idx_t IceDiskNodeTable::getNumTotalRows(const Transaction* transaction) {
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
        return metadata ? metadata->num_rows : 0;
    } catch (const std::exception& e) {
        // If parquet file is corrupted or invalid, return 0 instead of crashing
        return 0;
    }
}

size_t IceDiskNodeTable::getNumRowGroups(const transaction::Transaction* transaction) const {
    auto context = transaction->getClientContext();

    if (!context) {
        return 0;
    }

    std::vector<bool> columnSkips;

    try {
        auto resolvedPath = VirtualFileSystem::resolvePath(context, parquetFilePath);
        auto tempReader = std::make_unique<ParquetReader>(resolvedPath, columnSkips, context);
        return tempReader ? tempReader->getNumRowGroups() : 0;
    } catch (const std::exception& e) {
        // If parquet file is corrupted or invalid, return 0 instead of crashing
        return 0;
    }
}

size_t IceDiskNodeTable::getNumScanMorsels(const transaction::Transaction* transaction) const {
    return getNumRowGroups(transaction);
}

} // namespace storage
} // namespace lbug

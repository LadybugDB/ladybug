#include "storage/table/ice_disk_node_table.h"

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

const NodeTableCatalogEntry* IceDiskNodeTableScanState::getNodeTableCatalogEntry() const {
    return table->cast<IceDiskNodeTable>().getNodeTableCatalogEntry();
}

void IceDiskNodeTableScanState::setToTable(const Transaction* transaction, Table* table_,
    std::vector<column_id_t> columnIDs_, std::vector<ColumnPredicateSet> columnPredicateSets_,
    RelDataDirection /*direction*/) {
    table = table_;
    columnIDs = std::move(columnIDs_);
    columnPredicateSets = std::move(columnPredicateSets_);

    auto& iceDiskTable = table->cast<IceDiskNodeTable>();
    auto context = transaction->getClientContext();
    auto resolvedPath = VirtualFileSystem::resolvePath(context, iceDiskTable.getParquetFilePath());
    
    // Compute column skips
    if (columnSkips.empty()) {
        std::vector<bool> dummySkips;
        auto tempReader = std::make_unique<processor::ParquetReader>(resolvedPath, dummySkips, context);
        processor::ParquetReaderScanState tempState;
        std::vector<uint64_t> dummyGroups;
        tempReader->initializeScan(tempState, dummyGroups, VirtualFileSystem::GetUnsafe(*context));
        columnSkips.assign(tempReader->getNumColumns(), true);
        
        auto entry = iceDiskTable.getNodeTableCatalogEntry();
        for (size_t i = 0; i < columnIDs.size(); ++i) {
            auto columnID = columnIDs[i];
            if (columnID == INVALID_COLUMN_ID || columnID == ROW_IDX_COLUMN_ID) {
                continue;
            }

            auto propertyName = entry->getProperty(columnID).getName();
            for (uint32_t j = 0; j < tempReader->getNumColumns(); ++j) {
                if (tempReader->getColumnName(j) == propertyName) {
                    columnSkips[j] = false;
                    break;
                }
            }
        }
    }
    
    if (!parquetReader) {
        parquetReader = std::make_unique<ParquetReader>(resolvedPath, columnSkips, context);
    }
}

IceDiskNodeTable::IceDiskNodeTable(const StorageManager* storageManager,
    const NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager)
    : NodeTable{storageManager, nodeTableEntry, memoryManager},
      nodeTableCatalogEntry{nodeTableEntry},
      tableScanSharedState{std::make_unique<IceDiskNodeTableScanSharedState>()} {
    parquetFilePath = nodeTableEntry->getStorage() + "_nodes_" + nodeTableEntry->getName() + ".parquet";
}

void IceDiskNodeTable::initializeScanCoordination(const Transaction* transaction) {
    std::vector<size_t> rowGroupRows;
    std::vector<size_t> rowGroupStartRows;
    auto context = transaction->getClientContext();
    if (context) {
        std::vector<bool> dummySkips;
        auto resolvedPath = VirtualFileSystem::resolvePath(context, parquetFilePath);
        auto tempReader = std::make_unique<ParquetReader>(resolvedPath, dummySkips, context);
        auto metadata = tempReader->getMetadata();
        if (metadata) {
            uint64_t currentStartRow = 0;
            for (size_t i = 0; i < metadata->row_groups.size(); ++i) {
                rowGroupRows.push_back(metadata->row_groups[i].num_rows);
                rowGroupStartRows.push_back(currentStartRow);
                currentStartRow += metadata->row_groups[i].num_rows;
            }
        }
    }
    tableScanSharedState->reset(std::move(rowGroupRows), std::move(rowGroupStartRows));
}

void IceDiskNodeTable::initScanState(Transaction* /*transaction*/, TableScanState& scanState,
    bool /*resetCachedBoundNodeSelVec*/) const {
    auto& iceDiskNodeScanState = static_cast<IceDiskNodeTableScanState&>(scanState);
    iceDiskNodeScanState.source = TableScanSource::COMMITTED;
    iceDiskNodeScanState.scanCompleted = false;
    iceDiskNodeScanState.nodeGroupIdx = INVALID_NODE_GROUP_IDX;
}

bool IceDiskNodeTable::scanInternal(Transaction* transaction, TableScanState& scanState) {
    auto& iceDiskNodeScanState = static_cast<IceDiskNodeTableScanState&>(scanState);
    if (iceDiskNodeScanState.scanCompleted) {
        return false;
    }

    if (!tableScanSharedState->getNextMorsel(&iceDiskNodeScanState)) {
        iceDiskNodeScanState.scanCompleted = true;
        return false;
    }

    auto vfs = VirtualFileSystem::GetUnsafe(*transaction->getClientContext());
    std::vector<uint64_t> groupsToRead = {iceDiskNodeScanState.nodeGroupIdx};
    iceDiskNodeScanState.parquetReader->initializeScan(*iceDiskNodeScanState.parquetScanState,
        groupsToRead, vfs);

    DataChunk dataChunk(iceDiskNodeScanState.parquetReader->getNumColumns());
    for (uint32_t i = 0; i < iceDiskNodeScanState.parquetReader->getNumColumns(); ++i) {
        if (!iceDiskNodeScanState.columnSkips[i]) {
            dataChunk.insert(i, std::make_shared<ValueVector>(
                                   iceDiskNodeScanState.parquetReader->getColumnType(i).copy(),
                                   MemoryManager::Get(*transaction->getClientContext())));
        }
    }

    uint32_t outputCount = 0;
    while (iceDiskNodeScanState.parquetReader->scanInternal(*iceDiskNodeScanState.parquetScanState,
        dataChunk)) {
        auto selSize = dataChunk.state->getSelVector().getSelSize();
        // Copy to output vectors
        for (uint32_t i = 0; i < iceDiskNodeScanState.columnIDs.size(); ++i) {
            auto columnID = iceDiskNodeScanState.columnIDs[i];
            if (columnID == ROW_IDX_COLUMN_ID) {
                for (size_t j = 0; j < selSize; ++j) {
                    ((row_idx_t*)iceDiskNodeScanState.outputVectors[i]->getData())[outputCount + j] =
                        iceDiskNodeScanState.currentStartRow + outputCount + j;
                }
            } else if (columnID != INVALID_COLUMN_ID) {
                // Find parquet column index
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
                    dstVector.copyFromVectorData(outputCount + j, &srcVector, dataChunk.state->getSelVector()[j]);
                }
            }
        }
        outputCount += selSize;
    }

    // Set node IDs
    for (size_t i = 0; i < outputCount; ++i) {
        ((nodeID_t*)iceDiskNodeScanState.nodeIDVector->getData())[i] =
            nodeID_t{iceDiskNodeScanState.currentStartRow + i, nodeTableCatalogEntry->getTableID()};
    }
    iceDiskNodeScanState.outState->getSelVectorUnsafe().setSelSize(outputCount);
    iceDiskNodeScanState.outState->getSelVectorUnsafe().setToUnfiltered();

    return outputCount > 0;
}

common::row_idx_t IceDiskNodeTable::getNumTotalRows(const Transaction* transaction) {
    auto context = transaction->getClientContext();
    auto resolvedPath = VirtualFileSystem::resolvePath(context, parquetFilePath);
    std::vector<bool> dummySkips;
    processor::ParquetReader reader(resolvedPath, dummySkips, context);
    return reader.getMetadata()->num_rows;
}

} // namespace storage
} // namespace lbug

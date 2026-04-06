#include "storage/table/ice_disk_rel_table.h"
#include "storage/storage_manager.h"
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
    table = table_;
    columnIDs = std::move(columnIDs_);
    columnPredicateSets = std::move(columnPredicateSets_);
    direction = direction_;

    auto iceDiskRelTable = dynamic_cast<IceDiskRelTable*>(table_);
    auto context = transaction->getClientContext();
    auto resolvedPath = VirtualFileSystem::resolvePath(context, iceDiskRelTable->getIndicesFilePath());
    
    std::vector<bool> dummySkips;
    indicesReader = std::make_unique<processor::ParquetReader>(resolvedPath, dummySkips, context);
    auto tempState = std::make_unique<processor::ParquetReaderScanState>();
    std::vector<uint64_t> dummyGroups;
    indicesReader->initializeScan(*tempState, dummyGroups, VirtualFileSystem::GetUnsafe(*context));
    
    auto entry = iceDiskRelTable->getRelGroupCatalogEntry();
    propertyColumnIdx.clear();
    columnSkips.assign(indicesReader->getNumColumns(), true);
    
    for (auto columnID : columnIDs) {
        if (columnID == 0) { // NBR_ID
             bool found = false;
             for (uint32_t i = 0; i < indicesReader->getNumColumns(); i++) {
                 if (indicesReader->getColumnName(i) == "nbr_id" || i == 0) {
                     propertyColumnIdx.push_back(i);
                     columnSkips[i] = false;
                     found = true;
                     break;
                 }
             }
             if (!found) throw RuntimeException("nbr_id column not found in indices parquet");
        } else {
            auto propertyName = entry->getProperty(columnID).getName();
            bool found = false;
            for (uint32_t i = 0; i < indicesReader->getNumColumns(); i++) {
                if (indicesReader->getColumnName(i) == propertyName) {
                    propertyColumnIdx.push_back(i);
                    columnSkips[i] = false;
                    found = true;
                    break;
                }
            }
            if (!found) {
                 throw RuntimeException("Property " + propertyName + " not found in parquet file");
            }
        }
    }
    
    indicesReader = std::make_unique<processor::ParquetReader>(resolvedPath, columnSkips, context);
}

IceDiskRelTable::IceDiskRelTable(RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
    common::table_id_t toTableID, const StorageManager* storageManager,
    MemoryManager* memoryManager)
    : RelTable{relGroupEntry, fromTableID, toTableID, storageManager, memoryManager},
      relGroupCatalogEntry{relGroupEntry} {
    auto storage = relGroupEntry->getStorage();
    if (storage.substr(0, 12) == "icebug-disk:") {
        size_t commaPos = storage.find(',', 12);
        if (commaPos != std::string::npos) {
            indicesFilePath = storage.substr(12, commaPos - 12);
            indptrFilePath = storage.substr(commaPos + 1);
        } else {
             throw RuntimeException("Invalid icebug-disk storage string for rel table: " + storage);
        }
        indicesFilePath = storage + "_indices_" + relGroupEntry->getName() + ".parquet";
        indptrFilePath = storage + "_indptr_" + relGroupEntry->getName() + ".parquet";
    }
    tableScanSharedState = std::make_unique<IceDiskRelTableScanSharedState>();
}

void IceDiskRelTable::initializeScanCoordination(const Transaction* transaction) {
    if (tableScanSharedState->getNumMorsels() > 0) {
        return;
    }

    auto context = transaction->getClientContext();
    auto resolvedPath = VirtualFileSystem::resolvePath(context, indicesFilePath);
    std::vector<bool> dummySkips;
    processor::ParquetReader reader(resolvedPath, dummySkips, context);
    
    auto metadata = reader.getMetadata();
    std::vector<size_t> rowGroupStartRows;
    std::vector<size_t> rowGroupNumRows;
    size_t currentOffset = 0;
    
    for (auto i = 0u; i < metadata->row_groups.size(); ++i) {
        rowGroupStartRows.push_back(currentOffset);
        rowGroupNumRows.push_back(metadata->row_groups[i].num_rows);
        currentOffset += metadata->row_groups[i].num_rows;
    }
    
    tableScanSharedState->reset(rowGroupStartRows, rowGroupNumRows);
}

void IceDiskRelTable::initScanState(Transaction* transaction, TableScanState& scanState,
    bool resetCachedBoundNodeSelVec) const {
    RelTable::initScanState(transaction, scanState, resetCachedBoundNodeSelVec);
}

void IceDiskRelTable::loadIndptrData(Transaction* transaction) const {
    std::lock_guard<std::mutex> lock(indptrDataMutex);
    if (!indptrData.empty()) {
        return;
    }

    auto context = transaction->getClientContext();
    auto vfs = VirtualFileSystem::GetUnsafe(*context);
    auto resolvedPath = VirtualFileSystem::resolvePath(context, indptrFilePath);
    std::vector<bool> dummySkips;
    auto indptrReader = std::make_unique<processor::ParquetReader>(resolvedPath, dummySkips, context);
    
    auto scanState = std::make_unique<processor::ParquetReaderScanState>();
    std::vector<uint64_t> groupsToRead;
    for (uint64_t i = 0; i < indptrReader->getMetadata()->row_groups.size(); ++i) {
        groupsToRead.push_back(i);
    }
    indptrReader->initializeScan(*scanState, groupsToRead, vfs);
    
    DataChunk dataChunk(1);
    dataChunk.insert(0, std::make_shared<ValueVector>(LogicalType::UINT64(), MemoryManager::Get(*context)));
    
    while (indptrReader->scanInternal(*scanState, dataChunk)) {
        auto selSize = dataChunk.state->getSelVector().getSelSize();
        auto& vector = dataChunk.getValueVectorMutable(0);
        for (size_t i = 0; i < selSize; ++i) {
             indptrData.push_back(((uint64_t*)vector.getData())[dataChunk.state->getSelVector()[i]]);
        }
    }
}

common::offset_t IceDiskRelTable::findSourceNodeForRow(common::offset_t globalRowIdx) const {
    auto it = std::upper_bound(indptrData.cbegin(), indptrData.cend(), (common::offset_t)globalRowIdx);
    if (it == indptrData.cbegin()) {
        return INVALID_OFFSET;
    }
    return std::distance(indptrData.cbegin(), it) - 1;
}

bool IceDiskRelTable::scanInternal(Transaction* transaction, TableScanState& scanState) {
    auto& iceDiskScanState = static_cast<IceDiskRelTableScanState&>(scanState);
    if (iceDiskScanState.scanCompleted) {
        return false;
    }

    loadIndptrData(transaction);

    while (true) {
        uint64_t startRow, numRows;
        if (!tableScanSharedState->getNextMorsel(&iceDiskScanState, startRow, numRows)) {
            iceDiskScanState.scanCompleted = true;
            return false;
        }

        auto startNode = findSourceNodeForRow(startRow);
        auto endNode = findSourceNodeForRow(startRow + numRows - 1);
        
        bool overlap = false;
        for (size_t i = 0; i < iceDiskScanState.cachedBoundNodeSelVector.getSelSize(); ++i) {
            auto pos = iceDiskScanState.cachedBoundNodeSelVector[i];
            auto boundOffset = ((nodeID_t*)iceDiskScanState.nodeIDVector->getData())[pos].offset;
            if (boundOffset >= startNode && (startNode == endNode || (endNode != INVALID_OFFSET && boundOffset <= endNode))) {
                overlap = true;
                break;
            }
        }
        
        if (!overlap) {
            continue;
        }

        auto context = transaction->getClientContext();
        auto vfs = VirtualFileSystem::GetUnsafe(*context);
        std::vector<uint64_t> groupsToRead = {iceDiskScanState.nodeGroupIdx};
        iceDiskScanState.indicesReader->initializeScan(*iceDiskScanState.parquetScanState, groupsToRead, vfs);

        auto numColumns = iceDiskScanState.indicesReader->getNumColumns();
        DataChunk indicesChunk(numColumns);
        for (uint32_t i = 0; i < numColumns; ++i) {
             if (!iceDiskScanState.columnSkips[i]) {
                 indicesChunk.insert(i, std::make_shared<ValueVector>(iceDiskScanState.indicesReader->getColumnType(i).copy(), MemoryManager::Get(*context)));
             }
        }

        std::vector<nodeID_t> collectedNbrIDs;
        std::vector<std::vector<std::unique_ptr<Value>>> collectedProperties;
        collectedProperties.resize(iceDiskScanState.columnIDs.size());

        uint64_t currentRowIdx = startRow;
        while (iceDiskScanState.indicesReader->scanInternal(*iceDiskScanState.parquetScanState, indicesChunk)) {
            auto selSize = indicesChunk.state->getSelVector().getSelSize();
            for (size_t i = 0; i < selSize; ++i) {
                auto pos = indicesChunk.state->getSelVector()[i];
                auto srcOffset = findSourceNodeForRow(currentRowIdx + i);
                
                bool isBound = false;
                for (size_t j = 0; j < iceDiskScanState.cachedBoundNodeSelVector.getSelSize(); ++j) {
                    if (((nodeID_t*)iceDiskScanState.nodeIDVector->getData())[iceDiskScanState.cachedBoundNodeSelVector[j]].offset == srcOffset) {
                        isBound = true;
                        break;
                    }
                }

                if (isBound) {
                    offset_t dstOffset = ((offset_t*)indicesChunk.getValueVectorMutable(iceDiskScanState.propertyColumnIdx[0]).getData())[pos];
                    collectedNbrIDs.push_back(nodeID_t{dstOffset, getToNodeTableID()});
                    
                    for (size_t colIdx = 1; colIdx < iceDiskScanState.columnIDs.size(); ++colIdx) {
                        auto parquetColIdx = iceDiskScanState.propertyColumnIdx[colIdx];
                        auto& vec = indicesChunk.getValueVectorMutable(parquetColIdx);
                        collectedProperties[colIdx].push_back(vec.getAsValue(pos));
                    }
                }
            }
            currentRowIdx += selSize;
        }

        if (collectedNbrIDs.empty()) {
            continue;
        }

        auto nbrIDVector = iceDiskScanState.outputVectors[0];
        auto& outSelVector = iceDiskScanState.outState->getSelVectorUnsafe();
        outSelVector.setSelSize(collectedNbrIDs.size());
        for (size_t i = 0; i < collectedNbrIDs.size(); ++i) {
            outSelVector[i] = i;
            ((nodeID_t*)nbrIDVector->getData())[i] = collectedNbrIDs[i];
            
            for (size_t colIdx = 1; colIdx < iceDiskScanState.columnIDs.size(); ++colIdx) {
                iceDiskScanState.outputVectors[colIdx]->copyFromValue(i, *collectedProperties[colIdx][i]);
            }
        }
        
        return true;
    }
}

common::row_idx_t IceDiskRelTable::getNumTotalRows(const Transaction* transaction) {
    auto context = transaction->getClientContext();
    auto resolvedPath = VirtualFileSystem::resolvePath(context, indicesFilePath);
    std::vector<bool> dummySkips;
    processor::ParquetReader reader(resolvedPath, dummySkips, context);
    return reader.getMetadata()->num_rows;
}

} // namespace storage
} // namespace lbug

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

namespace {

constexpr int64_t REL_ID_OUTPUT_COLUMN = -2;

std::string resolveIceDiskPath(const std::string& storageRoot, const std::string& configuredPath,
    const std::string& fallbackPath) {
    if (configuredPath.empty()) {
        return fallbackPath;
    }
    auto configured = std::filesystem::path{configuredPath};
    if (configured.is_absolute()) {
        return configured.lexically_normal().string();
    }
    if (storageRoot.empty()) {
        return configured.lexically_normal().string();
    }
    auto baseDir = std::filesystem::path{storageRoot}.parent_path();
    if (baseDir.empty()) {
        return configured.lexically_normal().string();
    }
    return (baseDir / configured).lexically_normal().string();
}

std::string getRelPropertyNameForColumnID(const RelGroupCatalogEntry& entry, column_id_t columnID) {
    for (const auto& property : entry.getProperties()) {
        if (entry.getColumnID(property.getName()) == columnID) {
            return property.getName();
        }
    }
    throw RuntimeException("Column ID " + std::to_string(columnID) +
                           " does not map to an icebug-disk rel property.");
}

void copyCachedBoundNodeSelVector(RelTableScanState& relScanState) {
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

void emitPendingRow(IceDiskRelTableScanState& scanState) {
    auto& row = scanState.pendingRows[scanState.nextPendingRowIdx++];
    scanState.setNodeIDVectorToFlat(row.boundNodeSelPos);
    for (size_t outCol = 0; outCol < scanState.columnIDs.size(); ++outCol) {
        auto columnID = scanState.columnIDs[outCol];
        if (columnID == INVALID_COLUMN_ID || columnID == ROW_IDX_COLUMN_ID) {
            continue;
        }
        if (columnID == NBR_ID_COLUMN_ID) {
            scanState.outputVectors[outCol]->setValue<internalID_t>(0, row.nbrNodeID);
        } else if (columnID == REL_ID_COLUMN_ID) {
            scanState.outputVectors[outCol]->setValue<internalID_t>(0, row.relID);
        } else if (outCol < row.propertyValues.size() && row.propertyValues[outCol]) {
            scanState.outputVectors[outCol]->copyFromValue(0, *row.propertyValues[outCol]);
        }
    }
    scanState.outState->getSelVectorUnsafe().setToUnfiltered(1);
    if (scanState.nextPendingRowIdx >= scanState.pendingRows.size()) {
        scanState.pendingRows.clear();
        scanState.nextPendingRowIdx = 0;
    }
}

} // namespace

void IceDiskRelTableScanState::setToTable(const Transaction* transaction, Table* table_,
    std::vector<common::column_id_t> columnIDs_,
    std::vector<ColumnPredicateSet> columnPredicateSets_,
    common::RelDataDirection direction_) {
    table = table_;
    columnIDs = std::move(columnIDs_);
    columnPredicateSets = std::move(columnPredicateSets_);
    direction = direction_;

    auto& iceDiskRelTable = table_->cast<IceDiskRelTable>();
    auto context = transaction->getClientContext();
    auto resolvedPath = VirtualFileSystem::resolvePath(context, iceDiskRelTable.getIndicesFilePath());

    std::vector<bool> dummySkips;
    indicesReader = std::make_unique<processor::ParquetReader>(resolvedPath, dummySkips, context);
    auto tempState = std::make_unique<processor::ParquetReaderScanState>();
    std::vector<uint64_t> dummyGroups;
    indicesReader->initializeScan(*tempState, dummyGroups, VirtualFileSystem::GetUnsafe(*context));

    auto entry = iceDiskRelTable.getRelGroupCatalogEntry();
    outputColumnIdx.assign(columnIDs.size(), INVALID_COLUMN_ID);
    columnSkips.assign(indicesReader->getNumColumns(), true);

    for (size_t outputCol = 0; outputCol < columnIDs.size(); ++outputCol) {
        auto columnID = columnIDs[outputCol];
        if (columnID == INVALID_COLUMN_ID || columnID == ROW_IDX_COLUMN_ID) {
            continue;
        }
        if (columnID == NBR_ID_COLUMN_ID) {
            bool found = false;
            for (uint32_t i = 0; i < indicesReader->getNumColumns(); i++) {
                if (indicesReader->getColumnName(i) == "nbr_id" || i == 0) {
                    outputColumnIdx[outputCol] = static_cast<int64_t>(i);
                    columnSkips[i] = false;
                    found = true;
                    break;
                }
            }
            if (!found) {
                throw RuntimeException("nbr_id column not found in indices parquet");
            }
            continue;
        }
        if (columnID == REL_ID_COLUMN_ID) {
            outputColumnIdx[outputCol] = REL_ID_OUTPUT_COLUMN;
            continue;
        }

        auto propertyName = getRelPropertyNameForColumnID(*entry, columnID);
        bool found = false;
        for (uint32_t i = 0; i < indicesReader->getNumColumns(); i++) {
            if (indicesReader->getColumnName(i) == propertyName) {
                outputColumnIdx[outputCol] = static_cast<int64_t>(i);
                columnSkips[i] = false;
                found = true;
                break;
            }
        }
        if (!found) {
            throw RuntimeException("Property " + propertyName + " not found in parquet file");
        }
    }

    indicesReader = std::make_unique<processor::ParquetReader>(resolvedPath, columnSkips, context);
    processor::ParquetReaderScanState initializedState;
    indicesReader->initializeScan(initializedState, dummyGroups, VirtualFileSystem::GetUnsafe(*context));
}

IceDiskRelTable::IceDiskRelTable(RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
    common::table_id_t toTableID, const StorageManager* storageManager,
    MemoryManager* memoryManager)
    : RelTable{relGroupEntry, fromTableID, toTableID, storageManager, memoryManager},
      relGroupCatalogEntry{relGroupEntry} {
    auto storage = relGroupEntry->getStorage();
    indicesFilePath = resolveIceDiskPath(storage, relGroupEntry->getIndicesPath(),
        storage + "_indices_" + relGroupEntry->getName() + ".parquet");
    indptrFilePath = resolveIceDiskPath(storage, relGroupEntry->getIndptrPath(),
        storage + "_indptr_" + relGroupEntry->getName() + ".parquet");
    if (indicesFilePath.empty() || indptrFilePath.empty()) {
        throw RuntimeException("Invalid icebug-disk storage configuration for rel table: " +
                               relGroupEntry->getName());
    }
    tableScanSharedState = std::make_unique<IceDiskRelTableScanSharedState>();
}

void IceDiskRelTable::initializeScanCoordination(const Transaction* transaction) {
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

void IceDiskRelTable::initScanState(Transaction* /*transaction*/, TableScanState& scanState,
    bool resetCachedBoundNodeSelVec) const {
    auto& relScanState = scanState.cast<RelTableScanState>();
    relScanState.source = TableScanSource::COMMITTED;
    relScanState.nodeGroup = nullptr;
    relScanState.nodeGroupIdx = INVALID_NODE_GROUP_IDX;
    if (resetCachedBoundNodeSelVec) {
        copyCachedBoundNodeSelVector(relScanState);
    }

    auto& iceDiskScanState = static_cast<IceDiskRelTableScanState&>(relScanState);
    iceDiskScanState.scanCompleted = false;
    iceDiskScanState.currentStartRow = 0;
    iceDiskScanState.currentNumRows = 0;
    iceDiskScanState.currentGlobalRowIdx = 0;
    iceDiskScanState.nextRowGroupIdx = 0;
    iceDiskScanState.pendingRows.clear();
    iceDiskScanState.nextPendingRowIdx = 0;
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

    if (iceDiskScanState.nextPendingRowIdx < iceDiskScanState.pendingRows.size()) {
        emitPendingRow(iceDiskScanState);
        return true;
    }

    loadIndptrData(transaction);
    scanState.resetOutVectors();

    std::unordered_map<offset_t, sel_t> boundNodeSelPosByOffset;
    boundNodeSelPosByOffset.reserve(iceDiskScanState.cachedBoundNodeSelVector.getSelSize());
    for (size_t i = 0; i < iceDiskScanState.cachedBoundNodeSelVector.getSelSize(); ++i) {
        auto pos = iceDiskScanState.cachedBoundNodeSelVector[i];
        boundNodeSelPosByOffset.emplace(
            ((nodeID_t*)iceDiskScanState.nodeIDVector->getData())[pos].offset, pos);
    }

    auto context = transaction->getClientContext();
        auto vfs = VirtualFileSystem::GetUnsafe(*context);
        auto numColumns = iceDiskScanState.indicesReader->getNumColumns();
        DataChunk indicesChunk(numColumns);
    uint32_t nbrColumnIdx = 0;
    for (uint32_t i = 0; i < numColumns; ++i) {
        if (iceDiskScanState.indicesReader->getColumnName(i) == "nbr_id") {
            nbrColumnIdx = i;
            break;
        }
    }
    for (uint32_t i = 0; i < numColumns; ++i) {
        indicesChunk.insert(i, std::make_shared<ValueVector>(
                               iceDiskScanState.indicesReader->getColumnType(i).copy(),
                               MemoryManager::Get(*context)));
    }
    const auto nbrTableID =
        iceDiskScanState.direction == RelDataDirection::BWD ? getFromNodeTableID() : getToNodeTableID();

    while (true) {
        if (iceDiskScanState.nodeGroupIdx == INVALID_NODE_GROUP_IDX) {
            uint64_t startRow = 0;
            uint64_t numRows = 0;
            if (!tableScanSharedState->getMorsel(
                    static_cast<common::node_group_idx_t>(iceDiskScanState.nextRowGroupIdx),
                    startRow, numRows)) {
                iceDiskScanState.scanCompleted = true;
                return false;
            }
            iceDiskScanState.nodeGroupIdx =
                static_cast<common::node_group_idx_t>(iceDiskScanState.nextRowGroupIdx++);
            bool overlap = iceDiskScanState.direction == RelDataDirection::BWD;
            if (!overlap) {
                auto startNode = findSourceNodeForRow(startRow);
                auto endNode = findSourceNodeForRow(startRow + numRows - 1);
                for (const auto& [boundOffset, _] : boundNodeSelPosByOffset) {
                    if (boundOffset >= startNode &&
                        (startNode == endNode ||
                            (endNode != INVALID_OFFSET && boundOffset <= endNode))) {
                        overlap = true;
                        break;
                    }
                }
            }

            if (!overlap) {
                iceDiskScanState.nodeGroupIdx = INVALID_NODE_GROUP_IDX;
                continue;
            }

            iceDiskScanState.currentStartRow = startRow;
            iceDiskScanState.currentNumRows = numRows;
            iceDiskScanState.currentGlobalRowIdx = startRow;
            std::vector<uint64_t> groupsToRead = {iceDiskScanState.nodeGroupIdx};
            iceDiskScanState.indicesReader->initializeScan(*iceDiskScanState.parquetScanState,
                groupsToRead, vfs);
        }

        indicesChunk.state->getSelVectorUnsafe().setSelSize(0);
        iceDiskScanState.indicesReader->scan(*iceDiskScanState.parquetScanState, indicesChunk);
        if (indicesChunk.state->getSelVector().getSelSize() == 0) {
            iceDiskScanState.nodeGroupIdx = INVALID_NODE_GROUP_IDX;
            continue;
        }

        auto selSize = indicesChunk.state->getSelVector().getSelSize();
        for (size_t i = 0; i < selSize; ++i) {
            auto pos = indicesChunk.state->getSelVector()[i];
            auto globalRowIdx = iceDiskScanState.currentGlobalRowIdx + i;
            auto srcOffset = findSourceNodeForRow(globalRowIdx);
            auto& nbrVec = indicesChunk.getValueVectorMutable(nbrColumnIdx);
            auto dstOffset = nbrVec.getValue<offset_t>(pos);
            const auto boundOffset =
                iceDiskScanState.direction == RelDataDirection::BWD ? dstOffset : srcOffset;
            if (!boundNodeSelPosByOffset.contains(boundOffset)) {
                continue;
            }

            PendingIceDiskRelRow row;
            row.boundNodeSelPos = boundNodeSelPosByOffset.at(boundOffset);
            row.relID = internalID_t{globalRowIdx, getTableID()};
            row.propertyValues.resize(iceDiskScanState.columnIDs.size());
            for (size_t outCol = 0; outCol < iceDiskScanState.columnIDs.size(); ++outCol) {
                auto columnID = iceDiskScanState.columnIDs[outCol];
                if (columnID == INVALID_COLUMN_ID || columnID == ROW_IDX_COLUMN_ID ||
                    columnID == REL_ID_COLUMN_ID) {
                    continue;
                }
                auto parquetColIdx = iceDiskScanState.outputColumnIdx[outCol];
                if (parquetColIdx < 0) {
                    continue;
                }
                auto& vec = indicesChunk.getValueVectorMutable(static_cast<uint32_t>(parquetColIdx));
                if (columnID == NBR_ID_COLUMN_ID) {
                    auto nbrOffset =
                        iceDiskScanState.direction == RelDataDirection::BWD ? srcOffset : dstOffset;
                    row.nbrNodeID = internalID_t{nbrOffset, nbrTableID};
                } else {
                    row.propertyValues[outCol] = vec.getAsValue(pos);
                }
            }
            iceDiskScanState.pendingRows.push_back(std::move(row));
        }

        iceDiskScanState.currentGlobalRowIdx += selSize;
        if (iceDiskScanState.pendingRows.empty()) {
            continue;
        }
        emitPendingRow(iceDiskScanState);
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

#include "storage/table/ice_disk_rel_table.h"

#include <algorithm>

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/assert.h"
#include "common/constants.h"
#include "common/data_chunk/data_chunk.h"
#include "common/exception/runtime.h"
#include "common/file_system/virtual_file_system.h"
#include "common/types/internal_id_util.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"
#include "storage/storage_manager.h"
#include "storage/table/ice_disk_utils.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::transaction;
using namespace lbug::catalog;

namespace lbug {
namespace storage {

void IceDiskRelTableScanState::setToTable(const Transaction* transaction, Table* table_,
    std::vector<common::column_id_t> columnIDs_,
    std::vector<ColumnPredicateSet> columnPredicateSets_, common::RelDataDirection direction_) {
    TableScanState::setToTable(transaction, table_, std::move(columnIDs_),
        std::move(columnPredicateSets_));
    direction = direction_;
}

IceDiskRelTable::IceDiskRelTable(RelGroupCatalogEntry* relGroupEntry,
    common::table_id_t fromTableID, common::table_id_t toTableID,
    const StorageManager* storageManager, MemoryManager* memoryManager)
    : RelTable{relGroupEntry, fromTableID, toTableID, storageManager, memoryManager},
      relGroupCatalogEntry{relGroupEntry} {
    const auto base = IceDiskUtils::getBasePath(relGroupEntry->getStorage());
    auto paths = IceDiskUtils::constructCSRPaths(base, relGroupEntry->getName(), ".parquet");
    indicesFilePath = paths.indices;
    indptrFilePath = paths.indptr;
}

void IceDiskRelTable::initializeScanCoordination(Transaction* transaction) {
    loadIndptrData(transaction);
    loadIndicesMetadata(transaction);
}

void IceDiskRelTable::initScanState(Transaction* /*transaction*/, TableScanState& scanState,
    bool resetCachedBoundNodeSelVec) const {
    auto& relScanState = scanState.cast<RelTableScanState>();
    if (resetCachedBoundNodeSelVec) {
        copyCachedBoundNodeSelVector(relScanState);
    }
    relScanState.currBoundNodeIdx = 0;

    auto& iceState = dynamic_cast<IceDiskRelTableScanState&>(scanState);
    iceState.activeEdgePos = 0;
    iceState.activeEdgeEnd = 0;
}

bool IceDiskRelTable::scanInternal(Transaction* transaction, TableScanState& scanState) {
    auto& state = scanState.cast<RelTableScanState>();
    auto& iceState = dynamic_cast<IceDiskRelTableScanState&>(scanState);
    scanState.resetOutVectors();

    auto* context = transaction->getClientContext();
    auto* vfs = VirtualFileSystem::GetUnsafe(*context);
    auto* memMgr = MemoryManager::Get(*context);

    initIndicesReaderIfNeeded(iceState, context, vfs, memMgr);

    const bool isFwd = state.direction != RelDataDirection::BWD;
    const auto nbrTableID = isFwd ? getToNodeTableID() : getFromNodeTableID();
    const auto numBoundNodes = state.cachedBoundNodeSelVector.getSelSize();

    while (true) {
        // If the active node still has edges to emit, resume from where we left off.
        // Otherwise advance to the next bound node.
        if (iceState.activeEdgePos >= iceState.activeEdgeEnd) {
            if (state.currBoundNodeIdx >= numBoundNodes) {
                break;
            }
            const auto selPos = state.cachedBoundNodeSelVector[state.currBoundNodeIdx];
            const auto nodeOffset = state.nodeIDVector->getValue<nodeID_t>(selPos).offset;
            state.currBoundNodeIdx++;

            const auto range = getEdgeRange(nodeOffset, isFwd);
            if (!range) {
                iceState.activeEdgeEnd = 0;
                continue;
            }
            iceState.activeEdgePos = range->start;
            iceState.activeEdgeEnd = range->end;
            iceState.activeSelPos = selPos;
            iceState.activeNodeOffset = nodeOffset;
        }

        const auto [count, nextEdgePos] =
            collectNodeEdges(state, iceState, {iceState.activeEdgePos, iceState.activeEdgeEnd},
                iceState.activeNodeOffset, isFwd, nbrTableID, vfs);
        iceState.activeEdgePos = nextEdgePos;

        if (count == 0) {
            continue;
        }

        auto selVec = std::make_shared<SelectionVector>(static_cast<sel_t>(count));
        selVec->setToUnfiltered(static_cast<sel_t>(count));
        state.outState->setSelVector(selVec);
        state.setNodeIDVectorToFlat(iceState.activeSelPos);
        return true;
    }

    state.outState->setSelVector(std::make_shared<SelectionVector>(0));
    return false;
}

void IceDiskRelTable::initIndicesReaderIfNeeded(IceDiskRelTableScanState& iceState,
    main::ClientContext* context, VirtualFileSystem* vfs, MemoryManager* memMgr) const {
    if (iceState.indicesReader) {
        return;
    }
    auto resolvedPath = VirtualFileSystem::resolvePath(context, indicesFilePath);
    iceState.indicesReader =
        std::make_unique<processor::ParquetReader>(resolvedPath, std::vector<bool>(), context);
    // initializeScan triggers createReader() which populates column metadata.
    // Use an empty group list to get the schema only, without reading any data.
    iceState.indicesReader->initializeScan(*iceState.indicesScanState, {}, vfs);
    const uint32_t numCols = iceState.indicesReader->getNumColumns();
    iceState.scanBatch = std::make_unique<DataChunk>(numCols);
    for (uint32_t col = 0; col < numCols; ++col) {
        iceState.scanBatch->insert(col,
            std::make_shared<ValueVector>(iceState.indicesReader->getColumnType(col).copy(),
                memMgr));
    }
}

std::optional<IceDiskRelTable::EdgeRange> IceDiskRelTable::getEdgeRange(offset_t nodeOffset,
    bool isFwd) const {
    uint64_t start, end;
    if (isFwd) {
        if (nodeOffset + 1 >= indptrData.size()) {
            return std::nullopt;
        }
        start = indptrData[nodeOffset];
        end = indptrData[nodeOffset + 1];
    } else {
        start = 0;
        end = indicesRGStarts.empty() ? 0 : indicesRGStarts.back();
    }
    if (start >= end) {
        return std::nullopt;
    }
    return EdgeRange{start, end};
}

IceDiskRelTable::EdgeScanProgress IceDiskRelTable::collectNodeEdges(RelTableScanState& state,
    IceDiskRelTableScanState& iceState, EdgeRange range, offset_t nodeOffset, bool isFwd,
    table_id_t nbrTableID, VirtualFileSystem* vfs) const {
    // Locate the first row group containing range.start.
    auto it = std::upper_bound(indicesRGStarts.begin(), indicesRGStarts.end(), range.start);
    DASSERT(it != indicesRGStarts.begin());
    --it;
    const uint64_t startRG = static_cast<uint64_t>(std::distance(indicesRGStarts.begin(), it));

    // Collect all row groups covering [range.start, range.end).
    std::vector<uint64_t> rowGroups;
    for (uint64_t rg = startRG; rg + 1 < indicesRGStarts.size(); ++rg) {
        rowGroups.push_back(rg);
        if (indicesRGStarts[rg + 1] >= range.end) {
            break;
        }
    }
    iceState.indicesReader->initializeScan(*iceState.indicesScanState, rowGroups, vfs);

    uint64_t batchStart = indicesRGStarts[startRG];
    uint64_t count = 0;
    uint64_t nextEdgePos = range.end; // default: node fully scanned
    bool done = false;

    while (!done) {
        // Reset selSize before each scanInternal call: on a row-group transition scanInternal
        // returns true without writing data and without updating selSize, so the stale value
        // from the previous batch would otherwise be misread as real data.
        iceState.scanBatch->state->getSelVectorUnsafe().setSelSize(0);
        if (!iceState.indicesReader->scanInternal(*iceState.indicesScanState,
                *iceState.scanBatch)) {
            break;
        }
        const auto& batchSel = iceState.scanBatch->state->getSelVector();
        const auto batchSize = batchSel.getSelSize();
        const auto& batch = *iceState.scanBatch;

        for (uint64_t i = 0; i < batchSize; ++i) {
            const uint64_t globalRow = batchStart + i;
            if (globalRow < range.start) {
                continue;
            }
            if (globalRow >= range.end) {
                done = true;
                break;
            }

            const auto physIdx = batchSel[static_cast<sel_t>(i)];
            const auto destOffset = batch.getValueVector(0).getValue<offset_t>(physIdx);

            if (isFwd) {
                if (!state.outputVectors.empty()) {
                    state.outputVectors[0]->setValue(count, internalID_t{destOffset, nbrTableID});
                }
            } else {
                if (destOffset != nodeOffset) {
                    continue;
                }
                if (!state.outputVectors.empty()) {
                    state.outputVectors[0]->setValue(count,
                        internalID_t{findSourceNodeForRow(globalRow), nbrTableID});
                }
            }

            for (uint32_t col = 1;
                 col < batch.getNumValueVectors() && col < state.outputVectors.size(); ++col) {
                const auto& vec = batch.getValueVector(col);
                if (vec.isNull(physIdx)) {
                    state.outputVectors[col]->setNull(count, true);
                } else {
                    state.outputVectors[col]->copyFromValue(count, *vec.getAsValue(physIdx));
                }
            }

            if (++count >= DEFAULT_VECTOR_CAPACITY) {
                // Node has more edges; resume from the next global row on the next call.
                nextEdgePos = globalRow + 1;
                done = true;
                break;
            }
        }
        batchStart += batchSize;
    }

    return {count, nextEdgePos};
}

common::row_idx_t IceDiskRelTable::getNumTotalRows(const Transaction* transaction) {
    auto context = transaction->getClientContext();
    auto resolvedPath = VirtualFileSystem::resolvePath(context, indicesFilePath);
    processor::ParquetReader reader(resolvedPath, std::vector<bool>(), context);
    return reader.getMetadata()->num_rows;
}

void IceDiskRelTable::loadIndptrData(Transaction* transaction) {
    indptrData.clear();

    auto context = transaction->getClientContext();
    auto* vfs = VirtualFileSystem::GetUnsafe(*context);
    auto resolvedPath = VirtualFileSystem::resolvePath(context, indptrFilePath);
    auto reader =
        std::make_unique<processor::ParquetReader>(resolvedPath, std::vector<bool>(), context);
    processor::ParquetReaderScanState scanState;

    std::vector<uint64_t> groupsToRead;
    for (uint64_t i = 0; i < reader->getMetadata()->row_groups.size(); ++i) {
        groupsToRead.push_back(i);
    }
    reader->initializeScan(scanState, groupsToRead, vfs);

    if (reader->getNumColumns() == 0) {
        throw RuntimeException("Indptr parquet file has no columns");
    }
    if (!LogicalTypeUtils::isIntegral(reader->getColumnType(0).getLogicalTypeID())) {
        throw RuntimeException("Indptr parquet file column must be integer type");
    }

    DataChunk chunk(1);
    chunk.insert(0, std::make_shared<ValueVector>(reader->getColumnType(0).copy()));

    while (true) {
        // Reset selSize before each call so row-group transition calls (which return true
        // without updating selSize) are not mistaken for a stale data batch.
        chunk.state->getSelVectorUnsafe().setSelSize(0);
        if (!reader->scanInternal(scanState, chunk)) {
            break;
        }
        auto& sel = chunk.state->getSelVector();
        for (size_t i = 0; i < sel.getSelSize(); ++i) {
            indptrData.push_back(chunk.getValueVector(0).getValue<std::size_t>(sel[i]));
        }
    }
}

void IceDiskRelTable::loadIndicesMetadata(Transaction* transaction) {
    indicesRGStarts.clear();
    auto context = transaction->getClientContext();
    auto resolvedPath = VirtualFileSystem::resolvePath(context, indicesFilePath);
    processor::ParquetReader reader(resolvedPath, std::vector<bool>(), context);
    uint64_t cumulative = 0;
    for (const auto& rg : reader.getMetadata()->row_groups) {
        indicesRGStarts.push_back(cumulative);
        cumulative += static_cast<uint64_t>(rg.num_rows);
    }
    indicesRGStarts.push_back(cumulative); // sentinel = total edge count
}

void IceDiskRelTable::copyCachedBoundNodeSelVector(RelTableScanState& relScanState) const {
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

std::size_t IceDiskRelTable::findSourceNodeForRow(std::size_t globalRowIdx) const {
    auto it = std::upper_bound(indptrData.begin(), indptrData.end(), globalRowIdx);
    DASSERT(it != indptrData.begin());
    --it;
    return static_cast<std::size_t>(std::distance(indptrData.begin(), it));
}

} // namespace storage
} // namespace lbug

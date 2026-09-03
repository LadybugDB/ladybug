#include "processor/operator/scan/scan_node_table.h"

#include "binder/expression/expression_util.h"
#include "common/file_system/virtual_file_system.h"
#include "main/client_context.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/local_storage/local_node_table.h"
#include "storage/local_storage/local_storage.h"
#include "storage/table/arrow_node_table.h"
#include "storage/table/ice_disk_node_table.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace processor {

static constexpr double MORSELS_PER_THREAD_TARGET = 8.0;

std::unique_ptr<TableScanState> createNodeTableScanState(NodeTable* table,
    ValueVector* nodeIDVector, const std::vector<ValueVector*>& outVectors,
    MemoryManager* memoryManager) {
    if (dynamic_cast<IceDiskNodeTable*>(table) != nullptr) {
        return std::make_unique<IceDiskNodeTableScanState>(*memoryManager, nodeIDVector, outVectors,
            nodeIDVector->state);
    }
    if (dynamic_cast<ArrowNodeTable*>(table) != nullptr) {
        return std::make_unique<ArrowNodeTableScanState>(*memoryManager, nodeIDVector, outVectors,
            nodeIDVector->state);
    }
    return std::make_unique<NodeTableScanState>(nodeIDVector, outVectors, nodeIDVector->state);
}

std::string ScanNodeTablePrintInfo::toString() const {
    std::string result = "Tables: ";
    for (auto& tableName : tableNames) {
        result += tableName;
        if (tableName != tableNames.back()) {
            result += ", ";
        }
    }
    if (!alias.empty()) {
        result += ",Alias: ";
        result += alias;
    }
    if (!properties.empty()) {
        result += ",Properties: ";
        result += binder::ExpressionUtil::toString(properties);
    }
    return result;
}

void ScanNodeTableSharedState::initialize(const transaction::Transaction* transaction,
    NodeTable* table, ScanNodeTableProgressSharedState& progressSharedState,
    main::ClientContext* context) {
    this->table = table;
    this->currentCommittedGroupIdx = 0;
    this->currentUnCommittedGroupIdx = 0;
    this->currentGroupNextRow = 0;
    this->committedMorselSize = common::INVALID_ROW_IDX;

    // Initialize table-specific scan coordination (e.g., for IceDiskNodeTable)
    table->initializeScanCoordination(transaction);

    if (const auto iceDiskTable = dynamic_cast<IceDiskNodeTable*>(table)) {
        // For ice-disk tables, set numCommittedNodeGroups to number of row groups
        std::vector<bool> columnSkips;
        try {
            auto context = transaction->getClientContext();
            auto resolvedPath =
                common::VirtualFileSystem::resolvePath(context, iceDiskTable->getParquetFilePath());
            auto tempReader =
                std::make_unique<processor::ParquetReader>(resolvedPath, columnSkips, context);
            this->numCommittedNodeGroups = tempReader->getNumRowGroups();
        } catch (const std::exception& e) {
            this->numCommittedNodeGroups = 1;
        }
    } else if (const auto arrowTable = dynamic_cast<ArrowNodeTable*>(table)) {
        // For Arrow tables, set numCommittedNodeGroups to number of morsels
        this->numCommittedNodeGroups =
            static_cast<common::node_group_idx_t>(arrowTable->getNumScanMorsels(transaction));
    } else {
        this->numCommittedNodeGroups = table->getNumCommittedNodeGroups();
    }
    if (transaction->isWriteTransaction()) {
        if (const auto localTable =
                transaction->getLocalStorage()->getLocalTable(this->table->getTableID())) {
            auto& localNodeTable = localTable->cast<LocalNodeTable>();
            this->numUnCommittedNodeGroups = localNodeTable.getNumNodeGroups();
        }
    }
    // Native tables only: when the table has fewer node groups than worker threads, one
    // morsel per node group leaves downstream traversals (rel scans, extends, builds)
    // mostly serial, because those pipelines can only draw parallelism from this scan's
    // morsels. Split each node group into smaller row-range morsels instead.
    auto numMorsels = numCommittedNodeGroups;
    if (numCommittedNodeGroups > 0 && dynamic_cast<ArrowNodeTable*>(table) == nullptr &&
        dynamic_cast<IceDiskNodeTable*>(table) == nullptr) {
        const auto maxNumThreads = context != nullptr ? context->getMaxNumThreadForExec() : 1;
        if (numCommittedNodeGroups < maxNumThreads) {
            common::row_idx_t totalRows = 0;
            committedGroupNumRows.reserve(numCommittedNodeGroups);
            for (auto groupIdx = 0u; groupIdx < numCommittedNodeGroups; groupIdx++) {
                const auto numRows = table->getNumTuplesInNodeGroup(groupIdx);
                committedGroupNumRows.push_back(numRows);
                totalRows += numRows;
            }
            // Aim for enough morsels to keep all workers busy without making the per-morsel
            // setup cost dominate. Morsels are aligned to chunked group capacity boundaries.
            const auto targetNumMorsels =
                static_cast<common::row_idx_t>(maxNumThreads * MORSELS_PER_THREAD_TARGET);
            auto morselSize = (totalRows + targetNumMorsels - 1) / targetNumMorsels;
            const auto alignment = common::StorageConfig::CHUNKED_NODE_GROUP_CAPACITY;
            morselSize = (morselSize + alignment - 1) / alignment * alignment;
            this->committedMorselSize = std::max<common::row_idx_t>(morselSize, alignment);
            numMorsels = 0;
            for (auto numRows : committedGroupNumRows) {
                numMorsels +=
                    (numRows == 0) ? 0 : (numRows + committedMorselSize - 1) / committedMorselSize;
            }
            numMorsels = std::max<common::node_group_idx_t>(numMorsels, 1);
        }
    }
    progressSharedState.numMorsels += numMorsels;
}

void ScanNodeTableSharedState::nextMorsel(TableScanState& scanState,
    ScanNodeTableProgressSharedState& progressSharedState) {
    std::unique_lock lck{mtx};

    // ColumnarNodeTables handle morsel assignment internally
    // TODO: icebug-disk tables https://github.com/LadybugDB/ladybug/issues/245
    if (const auto arrowTable = dynamic_cast<ArrowNodeTable*>(this->table)) {
        const auto tableSharedState = arrowTable->getTableScanSharedState();
        if (tableSharedState->getNextMorsel(static_cast<ColumnarNodeTableScanState*>(&scanState))) {
            scanState.source = TableScanSource::COMMITTED;
            progressSharedState.numMorselsScanned++;
        } else {
            scanState.source = TableScanSource::NONE;
        }

        return;
    }

    auto& nodeScanState = scanState.cast<NodeTableScanState>();
    if (currentCommittedGroupIdx < numCommittedNodeGroups) {
        nodeScanState.nodeGroupIdx = currentCommittedGroupIdx;
        progressSharedState.numMorselsScanned++;
        nodeScanState.source = TableScanSource::COMMITTED;
        if (committedMorselSize == common::INVALID_ROW_IDX) {
            // One node group per morsel.
            nodeScanState.scanStartRowInGroup = 0;
            nodeScanState.scanEndRowInGroup = common::INVALID_ROW_IDX;
            currentCommittedGroupIdx++;
        } else {
            // Sub-node-group morsel: a row range [start, start + morselSize) within the
            // current group. The last morsel of a group scans to the actual end of the
            // group, so it stays correct even if `committedGroupNumRows` is stale.
            const auto startRow = currentGroupNextRow;
            nodeScanState.scanStartRowInGroup = startRow;
            currentGroupNextRow += committedMorselSize;
            const auto numRowsInGroup = committedGroupNumRows[currentCommittedGroupIdx];
            if (currentGroupNextRow >= numRowsInGroup) {
                nodeScanState.scanEndRowInGroup = common::INVALID_ROW_IDX;
                currentCommittedGroupIdx++;
                currentGroupNextRow = 0;
            } else {
                nodeScanState.scanEndRowInGroup = startRow + committedMorselSize;
            }
        }
        return;
    }
    if (currentUnCommittedGroupIdx < numUnCommittedNodeGroups) {
        nodeScanState.nodeGroupIdx = currentUnCommittedGroupIdx;
        nodeScanState.source = TableScanSource::UNCOMMITTED;
        nodeScanState.scanStartRowInGroup = 0;
        nodeScanState.scanEndRowInGroup = common::INVALID_ROW_IDX;
        return;
    }
    nodeScanState.source = TableScanSource::NONE;
}

table_id_map_t<SemiMask*> ScanNodeTable::getSemiMasks() const {
    table_id_map_t<SemiMask*> result;
    DASSERT(tableInfos.size() == sharedStates.size());
    for (auto i = 0u; i < sharedStates.size(); ++i) {
        result.insert({tableInfos[i].table->getTableID(), sharedStates[i]->getSemiMask()});
    }
    return result;
}

void ScanNodeTableInfo::initScanState(TableScanState& scanState,
    const std::vector<ValueVector*>& outVectors, main::ClientContext* context) {
    auto transaction = transaction::Transaction::Get(*context);
    scanState.setToTable(transaction, table, columnIDs, copyVector(columnPredicates));
    initScanStateVectors(scanState, outVectors, MemoryManager::Get(*context));
}

void ScanNodeTable::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) {
    ScanTable::initLocalStateInternal(resultSet, context);
    nodeIDVector = resultSet->getValueVector(opInfo.nodeIDPos).get();

    currentTableIdx = 0;
    initCurrentTable(context);
}

void ScanNodeTable::initCurrentTable(ExecutionContext* context) {
    auto& currentInfo = tableInfos[currentTableIdx];
    scanState = createNodeTableScanState(currentInfo.table->ptrCast<NodeTable>(), nodeIDVector,
        outVectors, MemoryManager::Get(*context->clientContext));
    currentInfo.initScanState(*scanState, outVectors, context->clientContext);
    scanState->semiMask = sharedStates[currentTableIdx]->getSemiMask();
    // Call table->initScanState for IceDiskNodeTable or ArrowNodeTable
    if (dynamic_cast<IceDiskNodeTable*>(tableInfos[currentTableIdx].table) ||
        dynamic_cast<ArrowNodeTable*>(tableInfos[currentTableIdx].table)) {
        auto transaction = transaction::Transaction::Get(*context->clientContext);
        tableInfos[currentTableIdx].table->initScanState(transaction, *scanState);
    }
}

void ScanNodeTable::initGlobalStateInternal(ExecutionContext* context) {
    DASSERT(sharedStates.size() == tableInfos.size());
    for (auto i = 0u; i < tableInfos.size(); i++) {
        sharedStates[i]->initialize(transaction::Transaction::Get(*context->clientContext),
            tableInfos[i].table->ptrCast<NodeTable>(), *progressSharedState,
            context->clientContext);
    }
}

bool ScanNodeTable::getNextTuplesInternal(ExecutionContext* context) {
    const auto transaction = transaction::Transaction::Get(*context->clientContext);
    while (currentTableIdx < tableInfos.size()) {
        auto& info = tableInfos[currentTableIdx];
        while (info.table->scan(transaction, *scanState)) {
            const auto outputSize = scanState->outState->getSelVector().getSelSize();
            if (outputSize > 0) {
                info.castColumns();
                scanState->outState->setToUnflat();
                metrics->numOutputTuple.increase(outputSize);
                return true;
            }
        }
        sharedStates[currentTableIdx]->nextMorsel(*scanState, *progressSharedState);
        if (scanState->source == TableScanSource::NONE) {
            currentTableIdx++;
            if (currentTableIdx < tableInfos.size()) {
                initCurrentTable(context);
            }
        } else {
            info.table->initScanState(transaction, *scanState);
        }
    }
    return false;
}

double ScanNodeTable::getProgress(ExecutionContext* /*context*/) const {
    if (currentTableIdx >= tableInfos.size()) {
        return 1.0;
    }
    if (progressSharedState->numMorsels == 0) {
        return 0.0;
    }
    return static_cast<double>(progressSharedState->numMorselsScanned) /
           progressSharedState->numMorsels;
}

} // namespace processor
} // namespace lbug

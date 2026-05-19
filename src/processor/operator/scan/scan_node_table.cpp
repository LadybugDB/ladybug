#include "processor/operator/scan/scan_node_table.h"

#include "binder/expression/expression_util.h"
#include "common/file_system/virtual_file_system.h"
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
static std::unique_ptr<TableScanState> createNodeTableScanState(NodeTable* table,
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
    NodeTable* table, ScanNodeTableProgressSharedState& progressSharedState) {
    this->table = table;
    this->currentCommittedGroupIdx = 0;
    this->currentUnCommittedGroupIdx = 0;

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
    progressSharedState.numMorsels += numCommittedNodeGroups;
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
        nodeScanState.nodeGroupIdx = currentCommittedGroupIdx++;
        progressSharedState.numMorselsScanned++;
        nodeScanState.source = TableScanSource::COMMITTED;
        return;
    }
    if (currentUnCommittedGroupIdx < numUnCommittedNodeGroups) {
        nodeScanState.nodeGroupIdx = currentUnCommittedGroupIdx++;
        nodeScanState.source = TableScanSource::UNCOMMITTED;
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
            tableInfos[i].table->ptrCast<NodeTable>(), *progressSharedState);
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

#include "processor/operator/scan/scan_node_table.h"

#include "binder/expression/expression_util.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/local_storage/local_node_table.h"
#include "storage/local_storage/local_storage.h"
#include "storage/table/arrow_node_table.h"
#include "storage/table/parquet_node_table.h"
#include "common/file_system/virtual_file_system.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace processor {
namespace {

std::string resolveParquetPath(main::ClientContext* context, const std::string& path) {
    if (!context) {
        return path;
    }
    auto vfs = common::VirtualFileSystem::GetUnsafe(*context);
    if (!vfs) {
        return path;
    }
    auto paths = vfs->glob(context, path);
    if (!paths.empty()) {
        return paths.front();
    }
    return path;
}

} // namespace

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

    // Initialize table-specific scan coordination (e.g., for ParquetNodeTable)
    table->initializeScanCoordination(transaction);

    if (const auto parquetTable = dynamic_cast<ParquetNodeTable*>(table)) {
        // For parquet tables, set numCommittedNodeGroups to number of row groups
        std::vector<bool> columnSkips;
        try {
            auto context = transaction->getClientContext();
            auto resolvedPath = resolveParquetPath(context, parquetTable->getParquetFilePath());
            auto tempReader = std::make_unique<processor::ParquetReader>(
                resolvedPath, columnSkips, context);
            this->numCommittedNodeGroups = tempReader->getNumRowsGroups();
        } catch (const std::exception& e) {
            this->numCommittedNodeGroups = 1;
        }
    } else if (const auto arrowTable = dynamic_cast<ArrowNodeTable*>(table)) {
        // For Arrow tables, set numCommittedNodeGroups to number of batches
        this->numCommittedNodeGroups = arrowTable->getNumBatches(transaction);
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
    progressSharedState.numGroups += numCommittedNodeGroups;
}

void ScanNodeTableSharedState::nextMorsel(TableScanState& scanState,
    ScanNodeTableProgressSharedState& progressSharedState) {
    std::unique_lock lck{mtx};
    // Cast to NodeTableScanState since we know this is for node tables
    auto& nodeScanState = scanState.cast<NodeTableScanState>();
    if (currentCommittedGroupIdx < numCommittedNodeGroups) {
        nodeScanState.nodeGroupIdx = currentCommittedGroupIdx++;
        progressSharedState.numGroupsScanned++;
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
    KU_ASSERT(tableInfos.size() == sharedStates.size());
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
    auto nodeIDVector = resultSet->getValueVector(opInfo.nodeIDPos).get();

    // Check if the first table is a ParquetNodeTable or ArrowNodeTable and create appropriate scan
    // state
    auto* parquetTable = dynamic_cast<ParquetNodeTable*>(tableInfos[0].table);
    auto* arrowTable = dynamic_cast<ArrowNodeTable*>(tableInfos[0].table);
    if (parquetTable) {
        scanState = std::make_unique<ParquetNodeTableScanState>(
            *MemoryManager::Get(*context->clientContext), nodeIDVector, outVectors,
            nodeIDVector->state);
    } else if (arrowTable) {
        scanState =
            std::make_unique<ArrowNodeTableScanState>(*MemoryManager::Get(*context->clientContext),
                nodeIDVector, outVectors, nodeIDVector->state);
    } else {
        scanState =
            std::make_unique<NodeTableScanState>(nodeIDVector, outVectors, nodeIDVector->state);
    }

    currentTableIdx = 0;
    initCurrentTable(context);
}

void ScanNodeTable::initCurrentTable(ExecutionContext* context) {
    auto& currentInfo = tableInfos[currentTableIdx];
    currentInfo.initScanState(*scanState, outVectors, context->clientContext);
    scanState->semiMask = sharedStates[currentTableIdx]->getSemiMask();
    // Call table->initScanState for ParquetNodeTable or ArrowNodeTable
    if (dynamic_cast<ParquetNodeTable*>(tableInfos[currentTableIdx].table) ||
        dynamic_cast<ArrowNodeTable*>(tableInfos[currentTableIdx].table)) {
        auto transaction = transaction::Transaction::Get(*context->clientContext);
        tableInfos[currentTableIdx].table->initScanState(transaction, *scanState);
    }
}

void ScanNodeTable::initGlobalStateInternal(ExecutionContext* context) {
    KU_ASSERT(sharedStates.size() == tableInfos.size());
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
    if (progressSharedState->numGroups == 0) {
        return 0.0;
    }
    return static_cast<double>(progressSharedState->numGroupsScanned) /
           progressSharedState->numGroups;
}

} // namespace processor
} // namespace lbug

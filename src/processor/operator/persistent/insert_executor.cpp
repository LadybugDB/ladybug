#include "processor/operator/persistent/insert_executor.h"

#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/exception/runtime.h"
#include "processor/partition_routing.h"
#include "storage/partition_storage_registry.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::transaction;

namespace lbug {
namespace processor {

void NodeInsertInfo::init(const ResultSet& resultSet) {
    nodeIDVector = resultSet.getValueVector(nodeIDPos).get();
    for (auto& pos : columnsPos) {
        if (pos.isValid()) {
            columnVectors.push_back(resultSet.getValueVector(pos).get());
        } else {
            columnVectors.push_back(nullptr);
        }
    }
}

void NodeInsertInfo::updateNodeID(nodeID_t nodeID) const {
    DASSERT(nodeIDVector->state->getSelVector().getSelSize() == 1);
    auto pos = nodeIDVector->state->getSelVector()[0];
    nodeIDVector->setNull(pos, false);
    nodeIDVector->setValue<nodeID_t>(pos, nodeID);
}

nodeID_t NodeInsertInfo::getNodeID() const {
    auto& nodeIDSelVector = nodeIDVector->state->getSelVector();
    DASSERT(nodeIDSelVector.getSelSize() == 1);
    if (nodeIDVector->isNull(nodeIDSelVector[0])) {
        return {INVALID_OFFSET, INVALID_TABLE_ID};
    }
    return nodeIDVector->getValue<nodeID_t>(nodeIDSelVector[0]);
}

void NodeTableInsertInfo::init(const ResultSet& resultSet, main::ClientContext* context) {
    for (auto& evaluator : columnDataEvaluators) {
        evaluator->init(resultSet, context);
        columnDataVectors.push_back(evaluator->resultVector.get());
        columnIDs.push_back(columnIDs.size());
    }
    // Null table means every partition is routed remotely; PK validation is the wrapper's job.
    pkVector = table == nullptr ? nullptr : columnDataVectors[table->getPKColumnID()];
}

void NodeInsertExecutor::init(ResultSet* resultSet, const ExecutionContext* context) {
    info.init(*resultSet);
    tableInfo.init(*resultSet, context->clientContext);
}

static void writeColumnVector(ValueVector* columnVector, const ValueVector* dataVector) {
    auto& columnSelVector = columnVector->state->getSelVector();
    auto& dataSelVector = dataVector->state->getSelVector();
    DASSERT(columnSelVector.getSelSize() == 1 && dataSelVector.getSelSize() == 1);
    auto columnPos = columnSelVector[0];
    auto dataPos = dataSelVector[0];
    if (dataVector->isNull(dataPos)) {
        columnVector->setNull(columnPos, true);
    } else {
        columnVector->setNull(columnPos, false);
        columnVector->copyFromVectorData(columnPos, dataVector, dataPos);
    }
}

// TODO(Guodong/Xiyang): think we can reference data vector instead of copy.
static void writeColumnVectors(const std::vector<ValueVector*>& columnVectors,
    const std::vector<ValueVector*>& dataVectors) {
    DASSERT(columnVectors.size() == dataVectors.size());
    for (auto i = 0u; i < columnVectors.size(); ++i) {
        if (columnVectors[i] == nullptr) { // No need to project
            continue;
        }
        writeColumnVector(columnVectors[i], dataVectors[i]);
    }
}

static void writeColumnVectorsToNull(const std::vector<ValueVector*>& columnVectors) {
    for (auto i = 0u; i < columnVectors.size(); ++i) {
        auto columnVector = columnVectors[i];
        if (columnVector == nullptr) { // No need to project
            continue;
        }
        auto& columnSelVector = columnVector->state->getSelVector();
        DASSERT(columnSelVector.getSelSize() == 1);
        columnVector->setNull(columnSelVector[0], true);
    }
}

void NodeInsertExecutor::setNodeIDVectorToNonNull() const {
    info.nodeIDVector->setNull(info.nodeIDVector->state->getSelVector()[0], false);
}

uint64_t NodeInsertExecutor::currentPartitionIndex() const {
    auto* keyVector = tableInfo.columnDataVectors[tableInfo.partitionKeyColumnID];
    DASSERT(keyVector->state->getSelVector().getSelSize() == 1);
    std::vector<uint64_t> partitionIndexes;
    computePartitionIndexes(*keyVector, tableInfo.partitionTables.size(), partitionIndexes);
    return partitionIndexes[0];
}

// Resolves the target partition subgraph for the current row.
// - LIST parents route through `listRouter`, creating a new partition on first sight of a value.
// - Empty `partitionTables` means a plain single-table write -> the table itself.
// - Returns nullptr when the selected partition is routed remotely through the hooks;
//   callers must branch on that (insertRemotely / hook lookup).
storage::NodeTable* NodeInsertExecutor::resolveTargetTable(main::ClientContext* context) {
    if (tableInfo.partitionMethod == common::PartitionMethod::LIST &&
        tableInfo.parentTableID != common::INVALID_TABLE_ID) {
        // LIST parents grow their partition set on first sight of a new key value.
        auto* keyVector = tableInfo.columnDataVectors[tableInfo.partitionKeyColumnID];
        DASSERT(keyVector->state->getSelVector().getSelSize() == 1);
        if (tableInfo.listRouter == nullptr) {
            auto* transaction = Transaction::Get(*context);
            auto* parent = catalog::Catalog::Get(*context)
                               ->getTableCatalogEntry(transaction, tableInfo.parentTableID)
                               ->ptrCast<catalog::NodeTableCatalogEntry>();
            tableInfo.listRouter = std::make_unique<ListPartitionRouter>(context, parent);
        }
        const auto pos = keyVector->state->getSelVector()[0];
        if (keyVector->isNull(pos)) {
            throw RuntimeException("Cannot insert into a LIST-partitioned table with a NULL "
                                   "partition-key value.");
        }
        return tableInfo.listRouter->route(*keyVector, pos).table;
    }
    if (tableInfo.partitionTables.empty()) {
        return tableInfo.table;
    }
    // Null table = remotely routed partition; callers branch on that.
    return tableInfo.partitionTables[currentPartitionIndex()];
}

nodeID_t NodeInsertExecutor::insertRemotely(uint64_t index,
    transaction::Transaction* transaction) const {
    const auto* hooks = common::getPartitionRoutingHooks();
    if (hooks == nullptr || hooks->insertRow == nullptr) {
        throw RuntimeException(
            "Partition is routed remotely but no routing hooks with insertRow are installed.");
    }
    auto* keyVector = tableInfo.columnDataVectors[tableInfo.partitionKeyColumnID];
    return hooks->insertRow(hooks->context, tableInfo.partitionRefs[index],
        tableInfo.partitionHandles[index], transaction, keyVector, tableInfo.columnDataVectors);
}

storage::NodeTable* NodeInsertExecutor::resolveTableForNodeID(common::nodeID_t nodeID,
    main::ClientContext* context) const {
    if (tableInfo.partitionTables.empty()) {
        if (tableInfo.partitionMethod == common::PartitionMethod::LIST &&
            tableInfo.parentTableID != common::INVALID_TABLE_ID) {
            // LIST: the node may live in a partition created after this executor was cloned;
            // resolve by the table ID recorded in the node ID itself (own data file).
            auto* entry = catalog::Catalog::Get(*context)->getTableCatalogEntry(
                transaction::Transaction::Get(*context), nodeID.tableID);
            return storage::PartitionStorageRegistry::resolveNodeTable(context, *entry);
        }
        return tableInfo.table;
    }
    for (auto i = 0u; i < tableInfo.partitionTables.size(); ++i) {
        if (tableInfo.partitionChildIDs[i] != nodeID.tableID) {
            continue;
        }
        if (tableInfo.partitionTables[i] != nullptr) {
            return tableInfo.partitionTables[i];
        }
        // Remotely routed partition: the caller must go through the hooks.
        return nullptr;
    }
    return tableInfo.table;
}

nodeID_t NodeInsertExecutor::insert(main::ClientContext* context) {
    for (auto& evaluator : tableInfo.columnDataEvaluators) {
        evaluator->evaluate();
    }
    auto transaction = Transaction::Get(*context);
    auto* targetTable = resolveTargetTable(context);
    nodeID_t resultNodeID;
    if (targetTable == nullptr && !tableInfo.partitionTables.empty()) {
        // Remotely routed partition: the wrapper owns conflict handling and returns the
        // remotely-assigned node ID.
        resultNodeID = insertRemotely(currentPartitionIndex(), transaction);
    } else {
        if (checkConflict(transaction, targetTable)) {
            return info.getNodeID();
        }
        storage::NodeTableInsertState insertState{*info.nodeIDVector, *tableInfo.pkVector,
            tableInfo.columnDataVectors};
        targetTable->initInsertState(context, insertState);
        targetTable->insert(transaction, insertState);
        resultNodeID = info.getNodeID();
    }
    writeColumnVectors(info.columnVectors, tableInfo.columnDataVectors);
    return resultNodeID;
}

void NodeInsertExecutor::skipInsert() const {
    for (auto& evaluator : tableInfo.columnDataEvaluators) {
        evaluator->evaluate();
    }
    info.nodeIDVector->setNull(info.nodeIDVector->state->getSelVector()[0], false);
    writeColumnVectors(info.columnVectors, tableInfo.columnDataVectors);
}

void NodeInsertExecutor::skipInsert(nodeID_t nodeID, main::ClientContext* context) const {
    info.updateNodeID(nodeID);
    std::vector<column_id_t> columnIDs;
    std::vector<ValueVector*> outputVectors;
    for (auto i = 0u; i < info.columnVectors.size(); ++i) {
        if (info.columnVectors[i] == nullptr) {
            continue;
        }
        columnIDs.push_back(tableInfo.columnIDs[i]);
        outputVectors.push_back(info.columnVectors[i]);
    }
    if (outputVectors.empty()) {
        return;
    }
    auto transaction = Transaction::Get(*context);
    auto* table = resolveTableForNodeID(nodeID, context);
    if (table == nullptr) {
        // Remotely routed partition: fetch the row through the routing hooks.
        const auto* hooks = common::getPartitionRoutingHooks();
        if (hooks == nullptr || hooks->lookupRow == nullptr) {
            throw RuntimeException("Partition is routed remotely but no routing hooks with "
                                   "lookupRow are installed.");
        }
        for (auto i = 0u; i < tableInfo.partitionTables.size(); ++i) {
            if (tableInfo.partitionChildIDs[i] == nodeID.tableID) {
                hooks->lookupRow(hooks->context, tableInfo.partitionRefs[i],
                    tableInfo.partitionHandles[i], transaction, nodeID, outputVectors);
                return;
            }
        }
        return;
    }
    storage::NodeTableScanState scanState{info.nodeIDVector, std::move(outputVectors),
        info.nodeIDVector->state};
    scanState.setToTable(transaction, table, std::move(columnIDs), {});
    table->initScanState(transaction, scanState, nodeID.tableID, nodeID.offset);
    table->lookup(transaction, scanState);
}

bool NodeInsertExecutor::checkConflict(const Transaction* transaction,
    storage::NodeTable* table) const {
    if (info.conflictAction == ConflictAction::ON_CONFLICT_DO_NOTHING) {
        auto offset = table->validateUniquenessConstraint(transaction, tableInfo.columnDataVectors);
        if (offset != INVALID_OFFSET) {
            // Conflict. Skip insertion.
            info.updateNodeID({offset, table->getTableID()});
            return true;
        }
    }
    return false;
}

void RelInsertInfo::init(const ResultSet& resultSet) {
    srcNodeIDVector = resultSet.getValueVector(srcNodeIDPos).get();
    dstNodeIDVector = resultSet.getValueVector(dstNodeIDPos).get();
    for (auto& pos : columnsPos) {
        if (pos.isValid()) {
            columnVectors.push_back(resultSet.getValueVector(pos).get());
        } else {
            columnVectors.push_back(nullptr);
        }
    }
}

void RelTableInsertInfo::init(const ResultSet& resultSet, main::ClientContext* context) {
    for (auto& evaluator : columnDataEvaluators) {
        evaluator->init(resultSet, context);
        columnDataVectors.push_back(evaluator->resultVector.get());
    }
}

internalID_t RelTableInsertInfo::getRelID() const {
    auto relIDVector = columnDataVectors[0];
    auto& nodeIDSelVector = relIDVector->state->getSelVector();
    DASSERT(nodeIDSelVector.getSelSize() == 1);
    if (relIDVector->isNull(nodeIDSelVector[0])) {
        return {INVALID_OFFSET, INVALID_TABLE_ID};
    }
    return relIDVector->getValue<nodeID_t>(nodeIDSelVector[0]);
}

void RelInsertExecutor::init(ResultSet* resultSet, const ExecutionContext* context) {
    info.init(*resultSet);
    tableInfo.init(*resultSet, context->clientContext);
}

internalID_t RelInsertExecutor::insert(main::ClientContext* context) {
    DASSERT(info.srcNodeIDVector->state->getSelVector().getSelSize() == 1);
    DASSERT(info.dstNodeIDVector->state->getSelVector().getSelSize() == 1);
    auto srcNodeIDPos = info.srcNodeIDVector->state->getSelVector()[0];
    auto dstNodeIDPos = info.dstNodeIDVector->state->getSelVector()[0];
    if (info.srcNodeIDVector->isNull(srcNodeIDPos) || info.dstNodeIDVector->isNull(dstNodeIDPos)) {
        // No need to insert.
        writeColumnVectorsToNull(info.columnVectors);
        return tableInfo.getRelID();
    }
    for (auto i = 1u; i < tableInfo.columnDataEvaluators.size(); ++i) {
        tableInfo.columnDataEvaluators[i]->evaluate();
    }
    storage::RelTableInsertState insertState{*info.srcNodeIDVector, *info.dstNodeIDVector,
        tableInfo.columnDataVectors};
    tableInfo.table->initInsertState(context, insertState);
    tableInfo.table->insert(Transaction::Get(*context), insertState);
    writeColumnVectors(info.columnVectors, tableInfo.columnDataVectors);
    return tableInfo.getRelID();
}

void RelInsertExecutor::skipInsert() const {
    for (auto i = 1u; i < tableInfo.columnDataEvaluators.size(); ++i) {
        tableInfo.columnDataEvaluators[i]->evaluate();
    }
    writeColumnVectors(info.columnVectors, tableInfo.columnDataVectors);
}

} // namespace processor
} // namespace lbug

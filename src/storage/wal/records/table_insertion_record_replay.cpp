#include "common/exception/runtime.h"
#include "storage/local_storage/local_rel_table.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "storage/table/rel_table.h"
#include "storage/wal/wal_replayer.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace storage {

void WALReplayer::replayTableInsertionRecord(const WALRecord& walRecord) const {
    const auto& insertionRecord = walRecord.constCast<TableInsertionRecord>();
    switch (insertionRecord.tableType) {
    case TableType::NODE: {
        replayNodeTableInsertRecord(walRecord);
    } break;
    case TableType::REL: {
        replayRelTableInsertRecord(walRecord);
    } break;
    default: {
        throw RuntimeException("Invalid table type for insertion replay in WAL record.");
    }
    }
}

void WALReplayer::replayNodeTableInsertRecord(const WALRecord& walRecord) const {
    const auto& insertionRecord = walRecord.constCast<TableInsertionRecord>();
    const auto tableID = insertionRecord.tableID;
    auto& table = StorageManager::Get(clientContext)->getTable(tableID)->cast<NodeTable>();
    DASSERT(!insertionRecord.ownedVectors.empty());
    const auto anchorState = insertionRecord.ownedVectors[0]->state;
    const auto numNodes = anchorState->getSelVector().getSelSize();
    for (auto i = 0u; i < insertionRecord.ownedVectors.size(); i++) {
        insertionRecord.ownedVectors[i]->setState(anchorState);
    }
    std::vector<ValueVector*> propertyVectors(insertionRecord.ownedVectors.size());
    for (auto i = 0u; i < insertionRecord.ownedVectors.size(); i++) {
        propertyVectors[i] = insertionRecord.ownedVectors[i].get();
    }
    DASSERT(table.getPKColumnID() < insertionRecord.ownedVectors.size());
    auto& pkVector = *insertionRecord.ownedVectors[table.getPKColumnID()];
    const auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID());
    nodeIDVector->setState(anchorState);
    const auto insertState =
        std::make_unique<NodeTableInsertState>(*nodeIDVector, pkVector, propertyVectors);
    DASSERT(transaction::Transaction::Get(clientContext) &&
            transaction::Transaction::Get(clientContext)->isRecovery());
    table.initInsertState(&clientContext, *insertState);
    anchorState->getSelVectorUnsafe().setToFiltered(1);
    for (auto i = 0u; i < numNodes; i++) {
        anchorState->getSelVectorUnsafe()[0] = i;
        table.insert(transaction::Transaction::Get(clientContext), *insertState);
    }
}

void WALReplayer::replayRelTableInsertRecord(const WALRecord& walRecord) const {
    const auto& insertionRecord = walRecord.constCast<TableInsertionRecord>();
    const auto tableID = insertionRecord.tableID;
    auto& table = StorageManager::Get(clientContext)->getTable(tableID)->cast<RelTable>();
    DASSERT(!insertionRecord.ownedVectors.empty());
    const auto anchorState = insertionRecord.ownedVectors[0]->state;
    const auto numRels = anchorState->getSelVector().getSelSize();
    anchorState->getSelVectorUnsafe().setToFiltered(1);
    for (auto i = 0u; i < insertionRecord.ownedVectors.size(); i++) {
        insertionRecord.ownedVectors[i]->setState(anchorState);
    }
    std::vector<ValueVector*> propertyVectors;
    for (auto i = 0u; i < insertionRecord.ownedVectors.size(); i++) {
        if (i < LOCAL_REL_ID_COLUMN_ID) {
            continue;
        }
        propertyVectors.push_back(insertionRecord.ownedVectors[i].get());
    }
    const auto insertState = std::make_unique<RelTableInsertState>(
        *insertionRecord.ownedVectors[LOCAL_BOUND_NODE_ID_COLUMN_ID],
        *insertionRecord.ownedVectors[LOCAL_NBR_NODE_ID_COLUMN_ID], propertyVectors);
    DASSERT(transaction::Transaction::Get(clientContext) &&
            transaction::Transaction::Get(clientContext)->isRecovery());
    for (auto i = 0u; i < numRels; i++) {
        anchorState->getSelVectorUnsafe()[0] = i;
        table.initInsertState(&clientContext, *insertState);
        table.insert(transaction::Transaction::Get(clientContext), *insertState);
    }
}

} // namespace storage
} // namespace lbug

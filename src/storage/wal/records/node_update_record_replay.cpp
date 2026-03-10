#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "storage/wal/wal_replayer.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace storage {

void WALReplayer::replayNodeUpdateRecord(const WALRecord& walRecord) const {
    const auto& updateRecord = walRecord.constCast<NodeUpdateRecord>();
    const auto tableID = updateRecord.tableID;
    auto& table = StorageManager::Get(clientContext)->getTable(tableID)->cast<NodeTable>();
    const auto anchorState = updateRecord.ownedPropertyVector->state;
    DASSERT(anchorState->getSelVector().getSelSize() == 1);
    const auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID());
    nodeIDVector->setState(anchorState);
    nodeIDVector->setValue<internalID_t>(0,
        internalID_t{updateRecord.nodeOffset, updateRecord.tableID});
    const auto updateState = std::make_unique<NodeTableUpdateState>(updateRecord.columnID,
        *nodeIDVector, *updateRecord.ownedPropertyVector);
    DASSERT(transaction::Transaction::Get(clientContext) &&
            transaction::Transaction::Get(clientContext)->isRecovery());
    table.update(transaction::Transaction::Get(clientContext), *updateState);
}

} // namespace storage
} // namespace lbug

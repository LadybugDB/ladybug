#include "storage/storage_manager.h"
#include "storage/table/rel_table.h"
#include "storage/wal/wal_replayer.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace storage {

void WALReplayer::replayRelUpdateRecord(const WALRecord& walRecord) const {
    const auto& updateRecord = walRecord.constCast<RelUpdateRecord>();
    const auto tableID = updateRecord.tableID;
    auto& table = StorageManager::Get(clientContext)->getTable(tableID)->cast<RelTable>();
    const auto anchorState = updateRecord.ownedRelIDVector->state;
    DASSERT(anchorState == updateRecord.ownedSrcNodeIDVector->state &&
            anchorState == updateRecord.ownedSrcNodeIDVector->state &&
            anchorState == updateRecord.ownedPropertyVector->state);
    DASSERT(anchorState->getSelVector().getSelSize() == 1);
    const auto updateState = std::make_unique<RelTableUpdateState>(updateRecord.columnID,
        *updateRecord.ownedSrcNodeIDVector, *updateRecord.ownedDstNodeIDVector,
        *updateRecord.ownedRelIDVector, *updateRecord.ownedPropertyVector);
    DASSERT(transaction::Transaction::Get(clientContext) &&
            transaction::Transaction::Get(clientContext)->isRecovery());
    table.update(transaction::Transaction::Get(clientContext), *updateState);
}

} // namespace storage
} // namespace lbug

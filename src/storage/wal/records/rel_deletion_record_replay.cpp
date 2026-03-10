#include "storage/storage_manager.h"
#include "storage/table/rel_table.h"
#include "storage/wal/wal_replayer.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace storage {

void WALReplayer::replayRelDeletionRecord(const WALRecord& walRecord) const {
    const auto& deletionRecord = walRecord.constCast<RelDeletionRecord>();
    const auto tableID = deletionRecord.tableID;
    auto& table = StorageManager::Get(clientContext)->getTable(tableID)->cast<RelTable>();
    const auto anchorState = deletionRecord.ownedRelIDVector->state;
    DASSERT(anchorState->getSelVector().getSelSize() == 1);
    const auto deleteState =
        std::make_unique<RelTableDeleteState>(*deletionRecord.ownedSrcNodeIDVector,
            *deletionRecord.ownedDstNodeIDVector, *deletionRecord.ownedRelIDVector);
    DASSERT(transaction::Transaction::Get(clientContext) &&
            transaction::Transaction::Get(clientContext)->isRecovery());
    table.delete_(transaction::Transaction::Get(clientContext), *deleteState);
}

} // namespace storage
} // namespace lbug

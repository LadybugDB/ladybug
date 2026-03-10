#include "storage/storage_manager.h"
#include "storage/table/rel_table.h"
#include "storage/wal/wal_replayer.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace storage {

void WALReplayer::replayRelDetachDeletionRecord(const WALRecord& walRecord) const {
    const auto& deletionRecord = walRecord.constCast<RelDetachDeleteRecord>();
    const auto tableID = deletionRecord.tableID;
    auto& table = StorageManager::Get(clientContext)->getTable(tableID)->cast<RelTable>();
    DASSERT(transaction::Transaction::Get(clientContext) &&
            transaction::Transaction::Get(clientContext)->isRecovery());
    const auto anchorState = deletionRecord.ownedSrcNodeIDVector->state;
    DASSERT(anchorState->getSelVector().getSelSize() == 1);
    const auto dstNodeIDVector =
        std::make_unique<ValueVector>(LogicalType{LogicalTypeID::INTERNAL_ID});
    const auto relIDVector = std::make_unique<ValueVector>(LogicalType{LogicalTypeID::INTERNAL_ID});
    dstNodeIDVector->setState(anchorState);
    relIDVector->setState(anchorState);
    const auto deleteState = std::make_unique<RelTableDeleteState>(
        *deletionRecord.ownedSrcNodeIDVector, *dstNodeIDVector, *relIDVector);
    deleteState->detachDeleteDirection = deletionRecord.direction;
    table.detachDelete(transaction::Transaction::Get(clientContext), deleteState.get());
}

} // namespace storage
} // namespace lbug

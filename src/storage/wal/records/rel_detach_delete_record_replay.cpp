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
    const auto dstNodeIDVector =
        std::make_unique<ValueVector>(LogicalType{LogicalTypeID::INTERNAL_ID});
    const auto relIDVector = std::make_unique<ValueVector>(LogicalType{LogicalTypeID::INTERNAL_ID});
    const auto relState = std::make_shared<DataChunkState>();
    dstNodeIDVector->setState(relState);
    relIDVector->setState(relState);
    table.detachDeleteBatch(transaction::Transaction::Get(clientContext),
        *deletionRecord.ownedSrcNodeIDVector, *dstNodeIDVector, *relIDVector,
        deletionRecord.direction);
}

} // namespace storage
} // namespace lbug

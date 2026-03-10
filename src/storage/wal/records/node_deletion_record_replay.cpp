#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "storage/wal/wal_replayer.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace storage {

void WALReplayer::replayNodeDeletionRecord(const WALRecord& walRecord) const {
    const auto& deletionRecord = walRecord.constCast<NodeDeletionRecord>();
    const auto tableID = deletionRecord.tableID;
    auto& table = StorageManager::Get(clientContext)->getTable(tableID)->cast<NodeTable>();
    const auto anchorState = deletionRecord.ownedPKVector->state;
    DASSERT(anchorState->getSelVector().getSelSize() == 1);
    const auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID());
    nodeIDVector->setState(anchorState);
    nodeIDVector->setValue<internalID_t>(0,
        internalID_t{deletionRecord.nodeOffset, deletionRecord.tableID});
    const auto deleteState =
        std::make_unique<NodeTableDeleteState>(*nodeIDVector, *deletionRecord.ownedPKVector);
    DASSERT(transaction::Transaction::Get(clientContext) &&
            transaction::Transaction::Get(clientContext)->isRecovery());
    table.delete_(transaction::Transaction::Get(clientContext), *deleteState);
}

} // namespace storage
} // namespace lbug

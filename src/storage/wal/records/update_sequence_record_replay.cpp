#include "catalog/catalog.h"
#include "catalog/catalog_entry/sequence_catalog_entry.h"
#include "storage/storage_manager.h"
#include "storage/wal/wal_replayer.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace storage {

void WALReplayer::replayUpdateSequenceRecord(const WALRecord& walRecord) const {
    auto& sequenceEntryRecord = walRecord.constCast<UpdateSequenceRecord>();
    const auto sequenceID = sequenceEntryRecord.sequenceID;
    const auto entry =
        Catalog::Get(clientContext)
            ->getSequenceEntry(transaction::Transaction::Get(clientContext), sequenceID);
    entry->nextKVal(transaction::Transaction::Get(clientContext), sequenceEntryRecord.kCount);
}

} // namespace storage
} // namespace lbug

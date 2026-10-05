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
    auto catalog = Catalog::Get(clientContext);
    auto transaction = transaction::Transaction::Get(clientContext);
    // sequenceName is empty in records written before the field existed. The name is
    // logged precisely because implicit serial sequences have no create record, so their
    // entry IDs shift between logging and replay (e.g. ANY-graph infrastructure
    // materialized before a standalone graph WAL replays).
    SequenceCatalogEntry* entry = nullptr;
    if (!sequenceEntryRecord.sequenceName.empty()) {
        for (auto* candidate : catalog->getSequenceEntries(transaction)) {
            if (candidate->getName() == sequenceEntryRecord.sequenceName) {
                entry = candidate;
                break;
            }
        }
    }
    if (entry == nullptr) {
        const auto sequenceID =
            getReplayedEntryID(CatalogEntryType::SEQUENCE_ENTRY, sequenceEntryRecord.sequenceID);
        entry = catalog->getSequenceEntry(transaction, sequenceID);
    }
    entry->nextKVal(transaction, sequenceEntryRecord.kCount);
}

} // namespace storage
} // namespace lbug

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
    replaySequenceRecord(sequenceEntryRecord.sequenceID, sequenceEntryRecord.kCount, "");
}

void WALReplayer::replayUpdateSequenceNamedRecord(const WALRecord& walRecord) const {
    auto& sequenceEntryRecord = walRecord.constCast<UpdateSequenceNamedRecord>();
    replaySequenceRecord(sequenceEntryRecord.sequenceID, sequenceEntryRecord.kCount,
        sequenceEntryRecord.sequenceName);
}

void WALReplayer::replaySequenceRecord(common::sequence_id_t sequenceID, uint64_t kCount,
    const std::string& sequenceName) const {
    auto catalog = Catalog::Get(clientContext);
    auto transaction = transaction::Transaction::Get(clientContext);
    // The named variant logs the sequence name precisely because implicit serial
    // sequences have no create record, so their entry IDs shift between logging and
    // replay (e.g. ANY-graph infrastructure materialized before a standalone graph WAL
    // replays). Legacy UPDATE_SEQUENCE records carry no name and always resolve by
    // translated entry ID.
    SequenceCatalogEntry* entry = nullptr;
    if (!sequenceName.empty() && catalog->containsSequence(transaction, sequenceName)) {
        entry = catalog->getSequenceEntry(transaction, sequenceName, false);
        // The catalog's name index is case-insensitive; accept only an exact match.
        if (entry->getName() != sequenceName) {
            entry = nullptr;
        }
    }
    if (entry == nullptr) {
        const auto replayedSequenceID =
            getReplayedEntryID(CatalogEntryType::SEQUENCE_ENTRY, sequenceID);
        entry = catalog->getSequenceEntry(transaction, replayedSequenceID);
    }
    entry->nextKVal(transaction, kCount);
}

} // namespace storage
} // namespace lbug

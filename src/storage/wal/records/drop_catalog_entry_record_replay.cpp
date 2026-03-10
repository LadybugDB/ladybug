#include "catalog/catalog.h"
#include "catalog/catalog_entry/catalog_entry.h"
#include "storage/storage_manager.h"
#include "storage/wal/wal_replayer.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace storage {

void WALReplayer::replayDropCatalogEntryRecord(const WALRecord& walRecord) const {
    auto& dropEntryRecord = walRecord.constCast<DropCatalogEntryRecord>();
    auto catalog = Catalog::Get(clientContext);
    auto transaction = transaction::Transaction::Get(clientContext);
    const auto entryID = dropEntryRecord.entryID;
    switch (dropEntryRecord.entryType) {
    case CatalogEntryType::NODE_TABLE_ENTRY:
    case CatalogEntryType::REL_GROUP_ENTRY: {
        DASSERT(Catalog::Get(clientContext));
        catalog->dropTableEntry(transaction, entryID);
    } break;
    case CatalogEntryType::SEQUENCE_ENTRY: {
        catalog->dropSequence(transaction, entryID);
    } break;
    case CatalogEntryType::INDEX_ENTRY: {
        catalog->dropIndex(transaction, entryID);
    } break;
    case CatalogEntryType::SCALAR_MACRO_ENTRY: {
        catalog->dropMacroEntry(transaction, entryID);
    } break;
    case CatalogEntryType::GRAPH_ENTRY: {
    } break;
    default: {
        UNREACHABLE_CODE;
    }
    }
}

} // namespace storage
} // namespace lbug

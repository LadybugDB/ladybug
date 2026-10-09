#include "catalog/catalog.h"
#include "catalog/catalog_entry/catalog_entry.h"
#include "catalog/catalog_entry/index_catalog_entry.h"
#include "main/client_context.h"
#include "main/database.h"
#include "main/database_manager.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
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
    const auto entryID = getReplayedEntryID(dropEntryRecord.entryType, dropEntryRecord.entryID);
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
        // Mirror the live DROP path, which removes the storage-side holder in
        // addition to the catalog entry. Without this, a crash-interrupted DROP
        // leaves a loaded holder behind and the next same-name CREATE fails in
        // addIndex even though the catalog is clean (see #1152).
        const IndexCatalogEntry* droppedIndexEntry = nullptr;
        for (auto* indexEntry : catalog->getIndexEntries(transaction)) {
            if (indexEntry->getOID() == entryID) {
                droppedIndexEntry = indexEntry;
                break;
            }
        }
        // Preserve existing behavior when the entry is already gone.
        catalog->dropIndex(transaction, entryID);
        if (droppedIndexEntry != nullptr) {
            auto storageManager = StorageManager::Get(clientContext);
            const auto tableID = droppedIndexEntry->getTableID();
            if (storageManager->containsTable(tableID) &&
                storageManager->getTable(tableID)->getTableType() == common::TableType::NODE) {
                auto* nodeTable = storageManager->getTable(tableID)->ptrCast<storage::NodeTable>();
                if (nodeTable->getIndexHolder(droppedIndexEntry->getIndexName()).has_value()) {
                    nodeTable->dropIndex(droppedIndexEntry->getIndexName());
                }
            }
        }
    } break;
    case CatalogEntryType::SCALAR_MACRO_ENTRY: {
        catalog->dropMacroEntry(transaction, entryID);
    } break;
    case CatalogEntryType::GRAPH_ENTRY: {
        // Covers DROP GRAPH and the implicit subgraph entry a DROP TABLE or RENAME
        // TABLE removes. Replay mirrors the in-memory half of the runtime drop: the
        // entry goes so a later CREATE GRAPH of the same name replays, and a loaded
        // catalog unloads so the name cannot keep routing records to a dropped graph
        // and replayedEntryIDs cannot keep a sub-map keyed by it. Files stay
        // untouched: DROP GRAPH removed the dropped graph's files at runtime, and at
        // replay any files under the name belong to a recreated graph.
        //
        // A subgraph's create is never WAL-logged (createNodeTableSubgraph skips it),
        // so its recorded ID has no recorded->replayed translation, and the table or
        // ALTER record's own replay already removed the subgraph entry: those records
        // must stay no-ops. Replay assigns fresh IDs to everything it creates, so an
        // untranslated ID can collide with another replay-created graph's ID, and
        // acting on such a match would drop a graph the user never dropped. Only two
        // matches are safe: the translation a CREATE GRAPH replay recorded (a DROP
        // GRAPH of a graph created in this WAL), and a recorded ID below the ID floor
        // captured when the pass began (a persisted entry, whose ID cannot shift).
        // Tagged records are always subgraph drops from a session on the graph, and
        // any other untranslated ID at or above the floor is one too.
        if (!walRecord.ownerCatalogName.empty()) {
            break;
        }
        common::oid_t translatedEntryID = 0;
        const auto translated = tryGetReplayedEntryID(CatalogEntryType::GRAPH_ENTRY,
            dropEntryRecord.entryID, translatedEntryID);
        if (!translated && dropEntryRecord.entryID >= graphOIDReplayFloor) {
            break;
        }
        catalog::GraphCatalogEntry* droppedEntry = nullptr;
        for (auto* graphEntry : catalog->getGraphEntries(transaction)) {
            if (graphEntry->getOID() == (translated ? translatedEntryID : entryID)) {
                droppedEntry = graphEntry;
                break;
            }
        }
        if (droppedEntry != nullptr) {
            auto graphName = droppedEntry->getName();
            auto* dbManager = main::DatabaseManager::Get(clientContext);
            // Only the main catalog's graphs are registered in the manager. A graph
            // dropped from another graph's own catalog (a standalone session's nested
            // create) must not unload the outer, registered graph of the same name.
            if (dbManager != nullptr && catalog == clientContext.getDatabase()->getCatalog()) {
                auto unloadedCatalog = dbManager->unloadGraphCatalog(graphName);
                if (unloadedCatalog != nullptr) {
                    // The recovery transaction can hold undo records pointing into
                    // the catalog, so destruction waits until the replayer dies.
                    replayedEntryIDs.erase(unloadedCatalog.get());
                    retiredCatalogs.push_back(std::move(unloadedCatalog));
                }
            }
            catalog->dropGraph(transaction, graphName);
        }
    } break;
    default: {
        UNREACHABLE_CODE;
    }
    }
}

} // namespace storage
} // namespace lbug

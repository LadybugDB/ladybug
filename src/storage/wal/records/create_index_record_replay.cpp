#include "catalog/catalog.h"
#include "catalog/catalog_entry/index_catalog_entry.h"
#include "common/exception/runtime.h"
#include "storage/index/art_index.h"
#include "storage/partition_storage_registry.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "storage/wal/wal_replayer.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace storage {

void WALReplayer::replayCreateIndexRecord(WALRecord& walRecord) const {
    auto& record = walRecord.cast<CreateIndexRecord>();
    auto* catalog = Catalog::Get(clientContext);
    auto* trx = transaction::Transaction::Get(clientContext);
    auto* storageManager = StorageManager::Get(clientContext);
    auto* indexCatalogEntry = record.ownedCatalogEntry->ptrCast<IndexCatalogEntry>();
    const auto recordedIndexID = indexCatalogEntry->getOID();
    indexCatalogEntry->setTableID(
        getReplayedEntryID(CatalogEntryType::NODE_TABLE_ENTRY, indexCatalogEntry->getTableID()));
    if (!catalog->containsIndex(trx, indexCatalogEntry->getTableID(),
            indexCatalogEntry->getIndexName())) {
        const auto replayedIndexID = catalog->createIndex(trx, std::move(record.ownedCatalogEntry));
        recordReplayedEntryID(CatalogEntryType::INDEX_ENTRY, recordedIndexID, replayedIndexID);
    } else {
        auto* existingIndex = catalog->getIndex(trx, indexCatalogEntry->getTableID(),
            indexCatalogEntry->getIndexName());
        recordReplayedEntryID(CatalogEntryType::INDEX_ENTRY, recordedIndexID,
            existingIndex->getOID());
    }
    record.indexInfo->tableID =
        getReplayedEntryID(CatalogEntryType::NODE_TABLE_ENTRY, record.indexInfo->tableID);
    auto* table = storage::PartitionStorageRegistry::resolveNodeTableByID(&clientContext,
        record.indexInfo->tableID)
                      ->ptrCast<NodeTable>();
    if (table->getIndex(record.indexInfo->name).has_value()) {
        return;
    }
    if (record.indexInfo->indexType != ArtPrimaryKeyIndex::getIndexType().typeName) {
        throw RuntimeException("CREATE_INDEX_RECORD currently only supports ART indexes.");
    }
    auto index = ArtPrimaryKeyIndex::loadFromWAL(&clientContext, storageManager,
        std::move(*record.indexInfo), record.treeBytes);
    table->addIndex(std::move(index));
}

} // namespace storage
} // namespace lbug

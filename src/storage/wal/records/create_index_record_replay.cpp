#include "catalog/catalog.h"
#include "catalog/catalog_entry/index_catalog_entry.h"
#include "common/exception/runtime.h"
#include "storage/index/art_index.h"
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
    auto& indexCatalogEntry = record.ownedCatalogEntry->constCast<IndexCatalogEntry>();
    if (!catalog->containsIndex(trx, indexCatalogEntry.getTableID(),
            indexCatalogEntry.getIndexName())) {
        catalog->createIndex(trx, std::move(record.ownedCatalogEntry));
    }
    auto* table = storageManager->getTable(record.indexInfo->tableID)->ptrCast<NodeTable>();
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

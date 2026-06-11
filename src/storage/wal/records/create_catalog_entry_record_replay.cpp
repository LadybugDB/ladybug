#include "catalog/catalog.h"
#include "catalog/catalog_entry/index_catalog_entry.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/scalar_macro_catalog_entry.h"
#include "catalog/catalog_entry/sequence_catalog_entry.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "catalog/catalog_entry/type_catalog_entry.h"
#include "storage/index/art_index.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "storage/wal/wal_replayer.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace storage {

static void rebuildArtIndexFromCatalog(main::ClientContext& clientContext,
    StorageManager* storageManager, Catalog* catalog, transaction::Transaction* transaction,
    const IndexCatalogEntry& indexEntry) {
    if (indexEntry.getIndexType() != ArtPrimaryKeyIndex::getIndexType().typeName) {
        return;
    }
    auto propertyIDs = indexEntry.getPropertyIDs();
    DASSERT(propertyIDs.size() == 1);
    auto* tableEntry = catalog->getTableCatalogEntry(transaction, indexEntry.getTableID());
    auto& property = tableEntry->getProperty(propertyIDs[0]);
    auto columnID = tableEntry->getColumnID(propertyIDs[0]);
    auto* table = storageManager->getTable(indexEntry.getTableID())->ptrCast<NodeTable>();
    if (table->getIndex(indexEntry.getIndexName()).has_value()) {
        return;
    }
    auto isPrimary = false;
    if (tableEntry->getType() == CatalogEntryType::NODE_TABLE_ENTRY) {
        isPrimary =
            tableEntry->constCast<NodeTableCatalogEntry>().getPrimaryKeyID() == propertyIDs[0];
    }
    IndexInfo indexInfo{indexEntry.getIndexName(), indexEntry.getIndexType(),
        indexEntry.getTableID(), {columnID}, {property.getType().getPhysicalType()}, isPrimary,
        true};
    table->buildIndexAndAdd(&clientContext,
        ArtPrimaryKeyIndex::createNewIndex(std::move(indexInfo)));
}

void WALReplayer::replayCreateCatalogEntryRecord(WALRecord& walRecord) const {
    auto catalog = Catalog::Get(clientContext);
    auto transaction = transaction::Transaction::Get(clientContext);
    auto storageManager = StorageManager::Get(clientContext);
    auto& record = walRecord.cast<CreateCatalogEntryRecord>();
    switch (record.ownedCatalogEntry->getType()) {
    case CatalogEntryType::NODE_TABLE_ENTRY:
    case CatalogEntryType::REL_GROUP_ENTRY: {
        auto& entry = record.ownedCatalogEntry->constCast<TableCatalogEntry>();
        auto newEntry = catalog->createTableEntry(transaction,
            entry.getBoundCreateTableInfo(transaction, record.isInternal));
        storageManager->createTable(newEntry->ptrCast<TableCatalogEntry>(), &clientContext);
    } break;
    case CatalogEntryType::SCALAR_MACRO_ENTRY: {
        auto& macroEntry = record.ownedCatalogEntry->constCast<ScalarMacroCatalogEntry>();
        catalog->addScalarMacroFunction(transaction, macroEntry.getName(),
            macroEntry.getMacroFunction()->copy());
    } break;
    case CatalogEntryType::SEQUENCE_ENTRY: {
        auto& sequenceEntry = record.ownedCatalogEntry->constCast<SequenceCatalogEntry>();
        catalog->createSequence(transaction,
            sequenceEntry.getBoundCreateSequenceInfo(record.isInternal));
    } break;
    case CatalogEntryType::TYPE_ENTRY: {
        auto& typeEntry = record.ownedCatalogEntry->constCast<TypeCatalogEntry>();
        catalog->createType(transaction, typeEntry.getName(), typeEntry.getLogicalType().copy());
    } break;
    case CatalogEntryType::INDEX_ENTRY: {
        auto& indexEntry = record.ownedCatalogEntry->constCast<IndexCatalogEntry>();
        auto indexEntryCopy = indexEntry.copy();
        catalog->createIndex(transaction, std::move(record.ownedCatalogEntry));
        rebuildArtIndexFromCatalog(clientContext, storageManager, catalog, transaction,
            *indexEntryCopy);
    } break;
    case CatalogEntryType::GRAPH_ENTRY: {
        auto& graphEntry = record.ownedCatalogEntry->constCast<GraphCatalogEntry>();
        catalog->createGraph(transaction, graphEntry.getName(), graphEntry.isAnyGraphType());
    } break;
    default: {
        UNREACHABLE_CODE;
    }
    }
}

} // namespace storage
} // namespace lbug

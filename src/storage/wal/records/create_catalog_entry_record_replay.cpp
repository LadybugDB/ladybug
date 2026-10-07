#include "binder/ddl/bound_create_table_info.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/index_catalog_entry.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
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
        auto boundInfo = entry.getBoundCreateTableInfo(transaction, record.isInternal);
        const auto isRelGroup = entry.getType() == CatalogEntryType::REL_GROUP_ENTRY;
        if (isRelGroup) {
            for (auto& relTableInfo :
                boundInfo.extraInfo->ptrCast<binder::BoundExtraCreateRelTableGroupInfo>()
                    ->relTableInfos) {
                relTableInfo.nodePair.srcTableID = getReplayedEntryID(
                    CatalogEntryType::NODE_TABLE_ENTRY, relTableInfo.nodePair.srcTableID);
                relTableInfo.nodePair.dstTableID = getReplayedEntryID(
                    CatalogEntryType::NODE_TABLE_ENTRY, relTableInfo.nodePair.dstTableID);
            }
        }
        auto newEntry = catalog->createTableEntry(transaction, boundInfo);
        auto* newTableEntry = newEntry->ptrCast<TableCatalogEntry>();
        recordReplayedEntryID(entry.getType(), entry.getTableID(), newTableEntry->getTableID());
        if (isRelGroup) {
            const auto& recordedRelEntryInfos =
                entry.constCast<RelGroupCatalogEntry>().getRelEntryInfos();
            const auto& replayedRelEntryInfos =
                newTableEntry->constCast<RelGroupCatalogEntry>().getRelEntryInfos();
            DASSERT(recordedRelEntryInfos.size() == replayedRelEntryInfos.size());
            for (auto i = 0u; i < recordedRelEntryInfos.size(); i++) {
                recordReplayedEntryID(entry.getType(), recordedRelEntryInfos[i].oid,
                    replayedRelEntryInfos[i].oid);
            }
        }
        storageManager->createTable(newTableEntry, &clientContext);
    } break;
    case CatalogEntryType::SCALAR_MACRO_ENTRY: {
        auto& macroEntry = record.ownedCatalogEntry->constCast<ScalarMacroCatalogEntry>();
        catalog->addScalarMacroFunction(transaction, macroEntry.getName(),
            macroEntry.getMacroFunction()->copy());
    } break;
    case CatalogEntryType::SEQUENCE_ENTRY: {
        auto& sequenceEntry = record.ownedCatalogEntry->constCast<SequenceCatalogEntry>();
        const auto replayedSequenceID = catalog->createSequence(transaction,
            sequenceEntry.getBoundCreateSequenceInfo(record.isInternal));
        recordReplayedEntryID(CatalogEntryType::SEQUENCE_ENTRY, sequenceEntry.getOID(),
            replayedSequenceID);
    } break;
    case CatalogEntryType::TYPE_ENTRY: {
        auto& typeEntry = record.ownedCatalogEntry->constCast<TypeCatalogEntry>();
        catalog->createType(transaction, typeEntry.getName(), typeEntry.getLogicalType().copy());
    } break;
    case CatalogEntryType::INDEX_ENTRY: {
        auto* indexEntry = record.ownedCatalogEntry->ptrCast<IndexCatalogEntry>();
        indexEntry->setTableID(
            getReplayedEntryID(CatalogEntryType::NODE_TABLE_ENTRY, indexEntry->getTableID()));
        auto indexEntryCopy = indexEntry->copy();
        const auto recordedIndexID = indexEntry->getOID();
        const auto replayedIndexID =
            catalog->createIndex(transaction, std::move(record.ownedCatalogEntry));
        recordReplayedEntryID(CatalogEntryType::INDEX_ENTRY, recordedIndexID, replayedIndexID);
        rebuildArtIndexFromCatalog(clientContext, storageManager, catalog, transaction,
            *indexEntryCopy);
    } break;
    case CatalogEntryType::GRAPH_ENTRY: {
        auto& graphEntry = record.ownedCatalogEntry->constCast<GraphCatalogEntry>();
        catalog->createGraph(transaction, graphEntry.getName(), graphEntry.isAnyGraphType());
        // Graph-entry IDs shift when rolled-back DDL consumed IDs between logging and
        // replay, so a later GRAPH_ENTRY drop must translate through this map like every
        // other entry type.
        recordReplayedEntryID(CatalogEntryType::GRAPH_ENTRY, graphEntry.getOID(),
            catalog->getGraphEntry(transaction, graphEntry.getName())->getOID());
    } break;
    default: {
        UNREACHABLE_CODE;
    }
    }
}

} // namespace storage
} // namespace lbug

#include "storage/storage_manager.h"

#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/arrow/arrow.h"
#include "common/constants.h"
#include "common/enums/storage_format.h"
#include "common/file_system/virtual_file_system.h"
#include "common/partition_routing_hook.h"
#include "common/random_engine.h"
#include "common/serializer/in_mem_file_writer.h"
#include "main/attached_database.h"
#include "main/client_context.h"
#include "main/database.h"
#include "main/database_manager.h"
#include "main/db_config.h"
#include "storage/buffer_manager/buffer_manager.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/checkpointer.h"
#include "storage/index/art_index.h"
#include "storage/partition_storage_registry.h"
#include "storage/storage_utils.h"
#include "storage/table/arrow_node_table.h"
#include "storage/table/arrow_rel_table.h"
#include "storage/table/arrow_table_support.h"
#include "storage/table/foreign_rel_table.h"
#include "storage/table/ice_disk_node_table.h"
#include "storage/table/ice_disk_rel_table.h"
#include "storage/table/node_table.h"
#include "storage/table/rel_table.h"
#include "storage/wal/wal_replayer.h"
#include "transaction/transaction.h"
#include <format>

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::transaction;

namespace lbug {
namespace storage {

namespace {
Table* resolvePartitionTable(main::ClientContext* context, table_id_t tableID) {
    auto* sm = PartitionStorageRegistry::Get(context)->tryGet(tableID);
    if (sm == nullptr || !sm->containsTable(tableID)) {
        throw RuntimeException(std::format(
            "Checkpoint failed: partition table with id {} not found.", std::to_string(tableID)));
    }
    return sm->getTable(tableID);
}
} // namespace

// A partitioned parent is a logical table: it owns no physical storage, its partition subgraphs
// do. Storage iteration (checkpoint / serialize / rollback) must skip the parents and touch the
// partitions, which are ordinary node-table entries in the same catalog set.
static void erasePartitionedParents(std::vector<NodeTableCatalogEntry*>& entries) {
    std::erase_if(entries, [](const auto* e) { return e->isPartitioned(); });
}

StorageManager::StorageManager(const std::string& databasePath, bool readOnly, bool enableChecksums,
    MemoryManager& memoryManager, bool enableCompression, bool enableDefaultHashIndex,
    VirtualFileSystem* vfs)
    : databasePath{databasePath}, readOnly{readOnly}, dataFH{nullptr}, memoryManager{memoryManager},
      enableCompression{enableCompression}, enableDefaultHashIndex{enableDefaultHashIndex},
      vfs_{vfs} {
    wal = std::make_unique<WAL>(databasePath, readOnly, enableChecksums, vfs);
    shadowFile =
        std::make_unique<ShadowFile>(*memoryManager.getBufferManager(), vfs, this->databasePath);
    inMemory = main::DBConfig::isDBPathInMemory(databasePath);
    // Checkpoint intent/apply lock files (*.checkpoint.intent.lock and
    // *.checkpoint.apply.lock) are NOT created here.  All access, including subgraph
    // access, goes through the main database path returned by
    // clientContext.getDatabasePath().  The Checkpointer creates these files on demand
    // (with CREATE_IF_NOT_EXISTS) when a checkpoint starts via acquireCheckpointLocks().
    registerIndexType(PrimaryKeyIndex::getIndexType());
    registerIndexType(ArtPrimaryKeyIndex::getIndexType());
}

StorageManager::~StorageManager() = default;

void StorageManager::initDataFileHandle(VirtualFileSystem* vfs, main::ClientContext* context) {
    if (inMemory) {
        dataFH = memoryManager.getBufferManager()->getFileHandle(databasePath,
            FileHandle::O_PERSISTENT_FILE_IN_MEM, vfs, context);
    } else {
        auto flag = readOnly ? FileHandle::O_PERSISTENT_FILE_READ_ONLY :
                               FileHandle::O_PERSISTENT_FILE_CREATE_NOT_EXISTS;
        if (!readOnly) {
            flag |= FileHandle::O_LOCKED_PERSISTENT_FILE;
        }
        dataFH = memoryManager.getBufferManager()->getFileHandle(databasePath, flag, vfs, context);
        if (dataFH->getNumPages() == 0) {
            if (!readOnly) {
                // Reserve the first page for the database header.
                dataFH->getPageManager()->allocatePage();
                // Write a dummy database header page.
                const auto* initialHeader = getOrInitDatabaseHeader(*context);
                auto headerWriter =
                    std::make_shared<InMemFileWriter>(*MemoryManager::Get(*context));
                Serializer headerSerializer(headerWriter);
                initialHeader->serialize(headerSerializer);
                dataFH->getFileInfo()->writeFile(headerWriter->getPage(0).data(), LBUG_PAGE_SIZE,
                    StorageConstants::DB_HEADER_PAGE_IDX);
                dataFH->getFileInfo()->syncFile();
            }
        }
    }
}

void StorageManager::closeFileHandle() {
    if (dataFH != nullptr) {
        dataFH->resetFileInfo();
    }
}

Table* StorageManager::getTable(table_id_t tableID) {
    std::shared_lock lck{mtx};
    DASSERT(tables.contains(tableID));
    return tables.at(tableID).get();
}

bool StorageManager::containsTable(table_id_t tableID) const {
    std::shared_lock lck{mtx};
    return tables.contains(tableID);
}

std::optional<PlannerTableStats> StorageManager::getCachedPlannerTableStats(
    table_id_t tableID) const {
    std::shared_lock lck{plannerStatsMtx};
    if (!plannerStatsCache.contains(tableID)) {
        return {};
    }
    return plannerStatsCache.at(tableID).copy();
}

void StorageManager::setCachedPlannerTableStats(PlannerTableStats stats) {
    std::unique_lock lck{plannerStatsMtx};
    plannerStatsCache.insert_or_assign(stats.tableID, std::move(stats));
}

void StorageManager::clearCachedPlannerTableStats(std::optional<table_id_t> tableID) {
    std::unique_lock lck{plannerStatsMtx};
    if (tableID.has_value()) {
        plannerStatsCache.erase(tableID.value());
    } else {
        plannerStatsCache.clear();
    }
}

void StorageManager::setDatabasePath(const std::string& newPath) {
    databasePath = newPath;
    if (shadowFile != nullptr) {
        shadowFile->setDatabasePath(newPath);
    }
}

void StorageManager::recover(main::ClientContext& clientContext, bool throwOnWalReplayFailure,
    bool enableChecksums) {
    const auto walReplayer = std::make_unique<WALReplayer>(clientContext);
    walReplayer->replay(throwOnWalReplayFailure, enableChecksums);
}

namespace {

// True if the entry is a partition subgraph whose storage is routed elsewhere by
// an installed PartitionRoutingHooks. Such partitions keep full catalog metadata
// but own no local table/WAL/checkpoint state.
bool isRemotelyRoutedPartition(const catalog::TableCatalogEntry* entry) {
    if (entry->getType() != CatalogEntryType::NODE_TABLE_ENTRY) {
        return false;
    }
    auto* nodeEntry = entry->constPtrCast<catalog::NodeTableCatalogEntry>();
    if (!nodeEntry->isPartitionChild()) {
        return false;
    }
    const auto* hooks = common::getPartitionRoutingHooks();
    if (hooks == nullptr || hooks->locate == nullptr) {
        return false;
    }
    common::PartitionHandle handle = nullptr;
    return hooks->locate(hooks->context,
        common::PartitionRef{nodeEntry->getParentTableID(), nodeEntry->getPartitionIndex()},
        &handle);
}

} // namespace

void StorageManager::createNodeTable(NodeTableCatalogEntry* entry, main::ClientContext* context) {
    tableNameCache[entry->getTableID()] = entry->getName();

    // A partition subgraph claimed by the routing wrapper owns no local storage:
    // no table object, no WAL records, no checkpoint work.
    if (isRemotelyRoutedPartition(entry)) {
        return;
    }

    if (entry->getStorageFormat() != StorageFormat::NONE) {
        if (entry->getStorageFormat() == StorageFormat::ICEBUG_DISK) {
            // Create icebug-disk-backed node table
            tables[entry->getTableID()] =
                std::make_unique<IceDiskNodeTable>(this, entry, &memoryManager, context);
        } else {
            throw common::RuntimeException(
                "Unsupported storage format option for node table: " +
                std::to_string(static_cast<int>(entry->getStorageFormat())));
        }
    } else if (!entry->getStorage().empty()) {
        // Check if storage is Arrow backed
        if (entry->getStorage().substr(0, 8) == "arrow://") {
            // Extract Arrow ID from storage string
            std::string arrowId = entry->getStorage().substr(8);

            // Retrieve Arrow data from registry (as pointers to registry data)
            ArrowSchemaWrapper* schema = nullptr;
            std::vector<ArrowArrayWrapper>* arrays = nullptr;
            if (!ArrowTableSupport::getArrowData(arrowId, schema, arrays)) {
                throw common::RuntimeException("Failed to retrieve Arrow data for ID: " + arrowId);
            }

            // Create wrappers that reference registry memory while registry keeps ownership.
            ArrowSchemaWrapper schemaCopy = createShallowCopy(*schema);
            std::vector<ArrowArrayWrapper> arraysCopy;
            arraysCopy.reserve(arrays->size());
            for (const auto& arr : *arrays) {
                arraysCopy.push_back(createShallowCopy(arr));
            }

            // Create Arrow-backed node table
            tables[entry->getTableID()] = std::make_unique<ArrowNodeTable>(this, entry,
                &memoryManager, std::move(schemaCopy), std::move(arraysCopy), arrowId);
        } else {
            throw common::RuntimeException(
                "Unsupported storage option for node table: " + entry->getStorage());
        }
    } else {
        // Create regular node table
        tables[entry->getTableID()] = std::make_unique<NodeTable>(this, entry, &memoryManager);
    }
}

// TODO(Guodong): This API is added since storageManager doesn't provide an API to add a single
// rel table. We may have to refactor the existing StorageManager::createTable(TableCatalogEntry*
// entry).
void StorageManager::addRelTable(RelGroupCatalogEntry* entry, const RelTableCatalogInfo& info,
    main::ClientContext* context) {
    if (entry->getScanFunction().has_value()) {
        // Create foreign-backed rel table
        tables[info.oid] = std::make_unique<ForeignRelTable>(entry, info.nodePair.srcTableID,
            info.nodePair.dstTableID, this, &memoryManager, *entry->getScanFunction(),
            std::move(entry->getScanBindData().value()));
    } else if (entry->getStorageFormat() != StorageFormat::NONE) {
        if (entry->getStorageFormat() == StorageFormat::ICEBUG_DISK) {
            // Create icebug-disk-backed rel table
            tables[info.oid] = std::make_unique<IceDiskRelTable>(entry, info.nodePair.srcTableID,
                info.nodePair.dstTableID, this, &memoryManager, context);
        } else {
            throw common::RuntimeException(
                "Unsupported storage format option for rel table: " +
                std::to_string(static_cast<int>(entry->getStorageFormat())));
        }
    } else if (!entry->getStorage().empty()) {
        if (entry->getStorage().substr(0, 8) == "arrow://") {
            std::string arrowId = entry->getStorage().substr(8);
            ArrowRelTableData* relData = nullptr;
            if (!ArrowTableSupport::getArrowRelData(arrowId, relData)) {
                throw common::RuntimeException("Failed to retrieve Arrow data for ID: " + arrowId);
            }
            if (!tables.contains(info.nodePair.srcTableID) ||
                !tables.contains(info.nodePair.dstTableID)) {
                throw common::RuntimeException(
                    "Source or destination node table is not initialized for Arrow rel table");
            }
            auto* fromNodeTable = tables.at(info.nodePair.srcTableID)->ptrCast<NodeTable>();
            auto* toNodeTable = tables.at(info.nodePair.dstTableID)->ptrCast<NodeTable>();
            if (!fromNodeTable || !toNodeTable) {
                throw common::RuntimeException(
                    "Arrow rel table currently supports only regular node tables");
            }
            ArrowSchemaWrapper schemaCopy = createShallowCopy(relData->schema);
            std::vector<ArrowArrayWrapper> arraysCopy;
            arraysCopy.reserve(relData->arrays.size());
            for (const auto& arr : relData->arrays) {
                arraysCopy.push_back(createShallowCopy(arr));
            }
            ArrowSchemaWrapper indptrSchemaCopy = createShallowCopy(relData->indptrSchema);
            std::vector<ArrowArrayWrapper> indptrArraysCopy;
            indptrArraysCopy.reserve(relData->indptrArrays.size());
            for (const auto& arr : relData->indptrArrays) {
                indptrArraysCopy.push_back(createShallowCopy(arr));
            }
            tables[info.oid] = std::make_unique<ArrowRelTable>(entry, info.nodePair.srcTableID,
                info.nodePair.dstTableID, this, &memoryManager, fromNodeTable, toNodeTable,
                relData->layout, std::move(schemaCopy), std::move(arraysCopy),
                std::move(indptrSchemaCopy), std::move(indptrArraysCopy), arrowId,
                relData->dstColumnName);
        } else {
            throw common::RuntimeException(
                "Unsupported storage option for rel table: " + entry->getStorage());
        }
    } else {
        // Create regular rel table
        tables[info.oid] = std::make_unique<RelTable>(entry, info.nodePair.srcTableID,
            info.nodePair.dstTableID, this, &memoryManager);
    }
}

void StorageManager::createRelTableGroup(RelGroupCatalogEntry* entry,
    main::ClientContext* context) {
    for (auto& info : entry->getRelEntryInfos()) {
        addRelTable(entry, info, context);
    }
}

void StorageManager::createTable(TableCatalogEntry* entry, main::ClientContext* context) {
    std::unique_lock lck{mtx};
    switch (entry->getType()) {
    case CatalogEntryType::NODE_TABLE_ENTRY: {
        auto* nodeEntry = entry->ptrCast<NodeTableCatalogEntry>();
        if (nodeEntry->isPartitioned()) {
            // A partitioned parent is a logical table: it has no physical storage of its own.
            // Each partition gets its own StorageManager and data file
            // (<base>.<childName>.db); see docs/partitioning.md 6b. When partition-routing
            // hooks land (PR 829), consult locate() here and skip local storage for claimed
            // partitions.
            auto* catalog = Catalog::Get(*context);
            auto* registry = PartitionStorageRegistry::Get(context);
            for (auto childTableID : nodeEntry->getChildTableIDs()) {
                auto* child = catalog->getTableCatalogEntry(transaction::Transaction::Get(*context),
                    childTableID);
                // Children claimed by a routing wrapper own no local state: no file, no table
                // object, no WAL/checkpoint work. The wrapper is notified via its hooks.
                auto* childNode = child->ptrCast<NodeTableCatalogEntry>();
                if (registry->isRemotelyRouted(childNode->getParentTableID(),
                        childNode->getPartitionIndex())) {
                    continue;
                }
                auto& childSM = registry->getOrCreate(context, childTableID, child->getName());
                // Child entries are ordinary node tables -> takes the createNodeTable branch.
                childSM.createTable(child, context);
            }
        } else {
            createNodeTable(nodeEntry, context);
        }
    } break;
    case CatalogEntryType::REL_GROUP_ENTRY: {
        createRelTableGroup(entry->ptrCast<RelGroupCatalogEntry>(), context);
    } break;
    default: {
        UNREACHABLE_CODE;
    }
    }
}

WAL& StorageManager::getWAL() const {
    DASSERT(wal);
    return *wal;
}

ShadowFile& StorageManager::getShadowFile() const {
    DASSERT(shadowFile);
    return *shadowFile;
}

void StorageManager::reclaimDroppedIndexes() {
    std::shared_lock lck{mtx};
    auto* pageAllocator = dataFH->getPageManager();
    for (const auto& [tableID, table] : tables) {
        if (table->getTableType() != TableType::NODE) {
            continue;
        }
        table->cast<NodeTable>().reclaimDroppedIndexes(*pageAllocator);
    }
}

void StorageManager::reclaimDroppedTables(const Catalog& catalog) {
    std::unique_lock lck{mtx};
    std::vector<table_id_t> droppedTables;
    for (const auto& [tableID, table] : tables) {
        switch (table->getTableType()) {
        case TableType::NODE: {
            if (!catalog.containsTable(&DUMMY_CHECKPOINT_TRANSACTION, tableID, true)) {
                table->reclaimStorage(*dataFH->getPageManager());
                droppedTables.push_back(tableID);
            }
        } break;
        case TableType::REL: {
            auto& relTable = table->cast<RelTable>();
            auto relGroupID = relTable.getRelGroupID();
            if (!catalog.containsTable(&DUMMY_CHECKPOINT_TRANSACTION, relGroupID, true)) {
                table->reclaimStorage(*dataFH->getPageManager());
                droppedTables.push_back(tableID);
            } else {
                auto relGroupEntry =
                    catalog.getTableCatalogEntry(&DUMMY_CHECKPOINT_TRANSACTION, relGroupID);
                if (!relGroupEntry->cast<RelGroupCatalogEntry>().getRelEntryInfo(
                        relTable.getFromNodeTableID(), relTable.getToNodeTableID())) {
                    table->reclaimStorage(*dataFH->getPageManager());
                    droppedTables.push_back(tableID);
                }
            }
        }
        default: {
            // DO NOTHING.
        }
        }
    }
    for (auto tableID : droppedTables) {
        tables.erase(tableID);
        clearCachedPlannerTableStats(tableID);
    }
}

bool StorageManager::checkpoint(main::ClientContext* context, const Catalog& catalog,
    PageAllocator& pageAllocator) {
    bool hasChanges = false;
    auto nodeTableEntries = catalog.getNodeTableEntries(&DUMMY_CHECKPOINT_TRANSACTION);
    const auto relGroupEntries = catalog.getRelGroupEntries(&DUMMY_CHECKPOINT_TRANSACTION);
    // Partitioned parents hold no physical storage; only their partition subgraphs do.
    erasePartitionedParents(nodeTableEntries);

    std::shared_lock lck{mtx};
    for (const auto entry : nodeTableEntries) {
        const auto tableID = entry->getTableID();
        if (!tables.contains(tableID)) {
            // Partition children live in their own StorageManagers (phase-B per-partition
            // files); checkpoint them against their own page manager. Entries absent from the
            // registry were never created locally (e.g. remote-claimed via routing hooks).
            auto* registry = PartitionStorageRegistry::Get(context);
            auto* childSM = registry->tryGet(tableID);
            if (childSM == nullptr || !childSM->containsTable(tableID)) {
                continue;
            }
            hasChanges = childSM->getTable(tableID)->checkpoint(context, entry,
                             *childSM->getDataFH()->getPageManager()) ||
                         hasChanges;
            continue;
        }
        hasChanges = tables.at(tableID)->checkpoint(context, entry, pageAllocator) || hasChanges;
    }
    for (const auto entry : relGroupEntries) {
        for (auto& info : entry->getRelEntryInfos()) {
            if (!tables.contains(info.oid)) {
                throw RuntimeException(std::format(
                    "Checkpoint failed: table {} not found in storage manager.", entry->getName()));
            }
            hasChanges =
                tables.at(info.oid)->checkpoint(context, entry, pageAllocator) || hasChanges;
        }
        entry->vacuumColumnIDs(1);
    }
    lck.unlock();
    reclaimDroppedIndexes();
    reclaimDroppedTables(catalog);
    // Remove partition children (and their files) whose catalog entry is gone. Only the main
    // storage manager may sweep the registry: attached/graph catalogs don't contain the
    // partition table IDs and would treat every live partition as orphaned.
    if (this == context->getDatabase()->getStorageManager()) {
        PartitionStorageRegistry::Get(context)->dropAllNotInCatalog(context, catalog);
        PartitionStorageRegistry::Get(context)->reconcilePaths(context, catalog);
    }
    return hasChanges;
}

bool StorageManager::checkpoint(main::ClientContext* context, const Catalog& catalog,
    const Transaction& snapshotTxn, PageAllocator& pageAllocator,
    const std::unordered_map<table_id_t, uint64_t>& epochWatermarks) {
    bool hasChanges = false;
    auto nodeTableEntries = catalog.getNodeTableEntries(&snapshotTxn);
    const auto relGroupEntries = catalog.getRelGroupEntries(&snapshotTxn);
    // Partitioned parents hold no physical storage; only their partition subgraphs do.
    erasePartitionedParents(nodeTableEntries);

    std::shared_lock lck{mtx};
    for (const auto entry : nodeTableEntries) {
        const auto tableID = entry->getTableID();
        if (!tables.contains(tableID)) {
            // See overload above: partition children checkpoint against their own file.
            auto* registry = PartitionStorageRegistry::Get(context);
            auto* childSM = registry->tryGet(tableID);
            if (childSM == nullptr || !childSM->containsTable(tableID)) {
                continue;
            }
            const auto watermarkIt = epochWatermarks.find(tableID);
            const uint64_t watermark =
                watermarkIt != epochWatermarks.end() ? watermarkIt->second : 0;
            hasChanges = childSM->getTable(tableID)->checkpoint(context, entry,
                             *childSM->getDataFH()->getPageManager(), &snapshotTxn, watermark) ||
                         hasChanges;
            continue;
        }
        const auto watermarkIt = epochWatermarks.find(tableID);
        const uint64_t watermark = watermarkIt != epochWatermarks.end() ? watermarkIt->second : 0;
        hasChanges = tables.at(tableID)->checkpoint(context, entry, pageAllocator, &snapshotTxn,
                         watermark) ||
                     hasChanges;
    }
    for (const auto entry : relGroupEntries) {
        for (auto& info : entry->getRelEntryInfos()) {
            if (!tables.contains(info.oid)) {
                throw RuntimeException(std::format(
                    "Checkpoint failed: table {} not found in storage manager.", entry->getName()));
            }
            const auto watermarkIt = epochWatermarks.find(info.oid);
            const uint64_t watermark =
                watermarkIt != epochWatermarks.end() ? watermarkIt->second : 0;
            hasChanges = tables.at(info.oid)->checkpoint(context, entry, pageAllocator,
                             &snapshotTxn, watermark) ||
                         hasChanges;
        }
        entry->vacuumColumnIDs(1);
    }
    lck.unlock();
    reclaimDroppedIndexes();
    reclaimDroppedTables(catalog);
    // See the other overload: registry sweeps are main-storage-manager only.
    if (this == context->getDatabase()->getStorageManager()) {
        PartitionStorageRegistry::Get(context)->dropAllNotInCatalog(context, catalog);
        PartitionStorageRegistry::Get(context)->reconcilePaths(context, catalog);
    }
    return hasChanges;
}

std::unordered_map<table_id_t, uint64_t> StorageManager::captureChangeEpochs() const {
    std::shared_lock lck{mtx};
    std::unordered_map<table_id_t, uint64_t> epochs;
    for (const auto& [id, table] : tables) {
        epochs[id] = table->getChangeEpoch();
    }
    return epochs;
}

void StorageManager::finalizeCheckpoint() {
    dataFH->getPageManager()->finalizeCheckpoint();
}

void StorageManager::rollbackCheckpoint(const Catalog& catalog, main::ClientContext* context) {
    std::unique_lock lck{mtx};
    const auto nodeTableEntries = catalog.getNodeTableEntries(&DUMMY_CHECKPOINT_TRANSACTION);
    for (const auto tableEntry : nodeTableEntries) {
        // Partitioned parents hold no physical storage; only their partition subgraphs do.
        if (tableEntry->getType() == CatalogEntryType::NODE_TABLE_ENTRY &&
            tableEntry->ptrCast<NodeTableCatalogEntry>()->isPartitioned()) {
            continue;
        }
        if (!tables.contains(tableEntry->getTableID())) {
            // Partition children roll back against their own file's page manager.
            auto* childSM =
                PartitionStorageRegistry::Get(context)->tryGet(tableEntry->getTableID());
            if (childSM != nullptr && childSM->containsTable(tableEntry->getTableID())) {
                childSM->getTable(tableEntry->getTableID())->rollbackCheckpoint();
                childSM->getDataFH()->getPageManager()->rollbackCheckpoint();
            }
            continue;
        }
        tables.at(tableEntry->getTableID())->rollbackCheckpoint();
    }
    dataFH->getPageManager()->rollbackCheckpoint();
}

std::optional<std::reference_wrapper<const IndexType>> StorageManager::getIndexType(
    const std::string& typeName) const {
    for (auto& indexType : registeredIndexTypes) {
        if (StringUtils::caseInsensitiveEquals(indexType.typeName, typeName)) {
            return indexType;
        }
    }
    return std::nullopt;
}

void StorageManager::serialize(const Catalog& catalog, main::ClientContext* context,
    Serializer& ser) {
    std::shared_lock lck{mtx};
    auto nodeTableEntries = catalog.getNodeTableEntries(&DUMMY_CHECKPOINT_TRANSACTION);
    auto relGroupEntries = catalog.getRelGroupEntries(&DUMMY_CHECKPOINT_TRANSACTION);
    std::sort(nodeTableEntries.begin(), nodeTableEntries.end(),
        [](const auto& a, const auto& b) { return a->getTableID() < b->getTableID(); });
    std::sort(relGroupEntries.begin(), relGroupEntries.end(),
        [](const auto& a, const auto& b) { return a->getTableID() < b->getTableID(); });
    // Partitioned parents own no physical storage; their partitions are serialized individually.
    erasePartitionedParents(nodeTableEntries);
    ser.writeDebuggingInfo("num_node_tables");
    ser.write<uint64_t>(nodeTableEntries.size());
    for (const auto tableEntry : nodeTableEntries) {
        // Partition subgraphs claimed by the routing wrapper hold no local state to serialize.
        if (isRemotelyRoutedPartition(tableEntry)) {
            continue;
        }
        const auto tableID = tableEntry->getTableID();
        ser.writeDebuggingInfo("table_id");
        ser.write<table_id_t>(tableID);
        // Partition children serialize from their own StorageManager (phase-B per-partition
        // files); the snapshot layout itself is unchanged.
        if (!tables.contains(tableID)) {
            resolvePartitionTable(context, tableID)->serialize(ser);
            continue;
        }
        tables.at(tableID)->serialize(ser);
    }
    ser.writeDebuggingInfo("num_rel_groups");
    ser.write<uint64_t>(relGroupEntries.size());
    for (const auto entry : relGroupEntries) {
        const auto& relGroupEntry = entry->cast<RelGroupCatalogEntry>();
        ser.writeDebuggingInfo("rel_group_id");
        ser.write<table_id_t>(relGroupEntry.getTableID());
        ser.writeDebuggingInfo("num_inner_rel_tables");
        ser.write<uint64_t>(relGroupEntry.getNumRelTables());
        for (auto& info : relGroupEntry.getRelEntryInfos()) {
            DASSERT(tables.contains(info.oid));
            info.serialize(ser);
            tables.at(info.oid)->serialize(ser);
        }
    }
}

void StorageManager::serialize(const Catalog& catalog, const Transaction& snapshotTxn,
    main::ClientContext* context, Serializer& ser) {
    auto nodeTableEntries = catalog.getNodeTableEntries(&snapshotTxn);
    auto relGroupEntries = catalog.getRelGroupEntries(&snapshotTxn);
    std::sort(nodeTableEntries.begin(), nodeTableEntries.end(),
        [](const auto& a, const auto& b) { return a->getTableID() < b->getTableID(); });
    std::sort(relGroupEntries.begin(), relGroupEntries.end(),
        [](const auto& a, const auto& b) { return a->getTableID() < b->getTableID(); });
    // Partitioned parents own no physical storage; their partitions are serialized individually.
    erasePartitionedParents(nodeTableEntries);

    std::shared_lock lck{mtx};
    ser.writeDebuggingInfo("num_node_tables");
    ser.write<uint64_t>(nodeTableEntries.size());
    for (const auto tableEntry : nodeTableEntries) {
        // Partition subgraphs claimed by the routing wrapper hold no local state to serialize.
        if (isRemotelyRoutedPartition(tableEntry)) {
            continue;
        }
        const auto tableID = tableEntry->getTableID();
        ser.writeDebuggingInfo("table_id");
        ser.write<table_id_t>(tableID);
        // Partition children serialize from their own StorageManager (phase-B per-partition
        // files); the snapshot layout itself is unchanged.
        if (!tables.contains(tableID)) {
            resolvePartitionTable(context, tableID)->serialize(ser);
            continue;
        }
        tables.at(tableID)->serialize(ser);
    }
    ser.writeDebuggingInfo("num_rel_groups");
    ser.write<uint64_t>(relGroupEntries.size());
    for (const auto entry : relGroupEntries) {
        const auto& relGroupEntry = entry->cast<RelGroupCatalogEntry>();
        ser.writeDebuggingInfo("rel_group_id");
        ser.write<table_id_t>(relGroupEntry.getTableID());
        ser.writeDebuggingInfo("num_inner_rel_tables");
        ser.write<uint64_t>(relGroupEntry.getNumRelTables());
        for (auto& info : relGroupEntry.getRelEntryInfos()) {
            DASSERT(tables.contains(info.oid));
            info.serialize(ser);
            tables.at(info.oid)->serialize(ser);
        }
    }
}

void StorageManager::deserialize(main::ClientContext* context, const Catalog* catalog,
    Deserializer& deSer) {
    std::string key;
    deSer.validateDebuggingInfo(key, "num_node_tables");
    uint64_t numNodeTables = 0;
    deSer.deserializeValue<uint64_t>(numNodeTables);
    for (auto i = 0u; i < numNodeTables; i++) {
        deSer.validateDebuggingInfo(key, "table_id");
        table_id_t tableID = INVALID_TABLE_ID;
        deSer.deserializeValue<table_id_t>(tableID);
        if (!catalog->containsTable(&DUMMY_TRANSACTION, tableID)) {
            throw RuntimeException(
                std::format("Load table failed: table {} doesn't exist in catalog.", tableID));
        }
        DASSERT(!tables.contains(tableID));
        auto tableEntry = catalog->getTableCatalogEntry(&DUMMY_TRANSACTION, tableID)
                              ->ptrCast<NodeTableCatalogEntry>();
        if (tableEntry->isPartitionChild()) {
            // Partition children are constructed into their own data files (phase-B).
            auto& sm = PartitionStorageRegistry::Get(context)->getOrCreate(context, tableID,
                tableEntry->getName());
            if (!sm.containsTable(tableID)) {
                sm.createTable(tableEntry, context);
            }
            sm.getTable(tableID)->deserialize(context, &sm, deSer);
            continue;
        }
        tableNameCache[tableID] = tableEntry->getName();
        if (tableEntry->getStorageFormat() == StorageFormat::ICEBUG_DISK) {
            // Create icebug-disk-backed node table
            tables[tableID] =
                std::make_unique<IceDiskNodeTable>(this, tableEntry, &memoryManager, context);
        } else {
            // Create regular node table
            tables[tableID] = std::make_unique<NodeTable>(this, tableEntry, &memoryManager);
        }
        tables[tableID]->deserialize(context, this, deSer);
    }
    deSer.validateDebuggingInfo(key, "num_rel_groups");
    uint64_t numRelGroups = 0;
    deSer.deserializeValue<uint64_t>(numRelGroups);
    for (auto i = 0u; i < numRelGroups; i++) {
        deSer.validateDebuggingInfo(key, "rel_group_id");
        table_id_t relGroupID = INVALID_TABLE_ID;
        deSer.deserializeValue<table_id_t>(relGroupID);
        if (!catalog->containsTable(&DUMMY_TRANSACTION, relGroupID)) {
            throw RuntimeException(
                std::format("Load table failed: table {} doesn't exist in catalog.", relGroupID));
        }
        deSer.validateDebuggingInfo(key, "num_inner_rel_tables");
        uint64_t numInnerRelTables = 0;
        deSer.deserializeValue<uint64_t>(numInnerRelTables);
        auto relGroupEntry = catalog->getTableCatalogEntry(&DUMMY_TRANSACTION, relGroupID)
                                 ->ptrCast<RelGroupCatalogEntry>();
        for (auto k = 0u; k < numInnerRelTables; k++) {
            RelTableCatalogInfo info = RelTableCatalogInfo::deserialize(deSer);
            DASSERT(!tables.contains(info.oid));
            if (relGroupEntry->getStorageFormat() == StorageFormat::ICEBUG_DISK) {
                // Create icebug-disk-backed rel table
                tables[info.oid] =
                    std::make_unique<IceDiskRelTable>(relGroupEntry, info.nodePair.srcTableID,
                        info.nodePair.dstTableID, this, &memoryManager, context);
            } else {
                // Create regular rel table
                tables[info.oid] = std::make_unique<RelTable>(relGroupEntry,
                    info.nodePair.srcTableID, info.nodePair.dstTableID, this, &memoryManager);
            }
            tables.at(info.oid)->deserialize(context, this, deSer);
        }
    }
}

common::uuid StorageManager::getOrInitDatabaseID(const main::ClientContext& clientContext) {
    return getOrInitDatabaseHeader(clientContext)->databaseID;
}

const storage::DatabaseHeader* StorageManager::getOrInitDatabaseHeader(
    const main::ClientContext& clientContext) {
    if (databaseHeader == nullptr) {
        // We should only create the database header if a persistent one doesn't exist
        DASSERT(std::nullopt == DatabaseHeader::readDatabaseHeader(*dataFH->getFileInfo()));
        databaseHeader = std::make_unique<DatabaseHeader>(
            DatabaseHeader::createInitialHeader(RandomEngine::Get(clientContext)));
    }
    return databaseHeader.get();
}

void StorageManager::setDatabaseHeader(std::unique_ptr<storage::DatabaseHeader> header) {
    DASSERT(!databaseHeader || header->databaseID.value == databaseHeader->databaseID.value);
    databaseHeader = std::move(header);
}

StorageManager* StorageManager::Get(const main::ClientContext& context) {
    if (context.getAttachedDatabase()) {
        return context.getAttachedDatabase()->getStorageManager();
    }
    auto dbManager = main::DatabaseManager::Get(context);
    auto graphStorageManager = dbManager->getDefaultGraphStorageManager();
    if (graphStorageManager != nullptr) {
        return graphStorageManager;
    }
    return context.getDatabase()->getStorageManager();
}

} // namespace storage
} // namespace lbug

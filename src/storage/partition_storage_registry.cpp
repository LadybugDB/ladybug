#include "storage/partition_storage_registry.h"

#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/constants.h"
#include "common/exception/runtime.h"
#include "common/file_system/virtual_file_system.h"
#include "common/partition_routing_hook.h"
#include "common/serializer/buffered_file.h"
#include "common/serializer/deserializer.h"
#include "main/client_context.h"
#include "main/database.h"
#include "main/database_manager.h"
#include "main/db_config.h"
#include "storage/database_header.h"
#include "storage/shadow_file.h"
#include "storage/storage_manager.h"
#include "storage/storage_utils.h"
#include "storage/table/node_table.h"
#include "transaction/transaction.h"

using namespace lbug::catalog;
using namespace lbug::common;

namespace lbug {
namespace storage {

PartitionStorageRegistry* PartitionStorageRegistry::Get(main::ClientContext* context) {
    return main::DatabaseManager::Get(*context)->getPartitionStorageRegistry();
}

static std::string getChildPath(main::ClientContext* context, const std::string& childName) {
    auto dbPath = context->getDatabasePath();
    if (main::DBConfig::isDBPathInMemory(dbPath)) {
        // In-memory databases keep partitions in memory too; the path is only a registry key.
        return ":" + childName;
    }
    return StorageUtils::getGraphPath(dbPath, childName);
}

// Restore the child file's page manager from its own header. The checkpointer persists each
// partition child's free-space state into its file (persistPartitionChildFiles); without this
// reload, a freshly opened child would hand out page indices that overwrite existing data.
static bool loadChildHeaderAndPageManager(StorageManager& sm) {
    auto* dataFH = sm.getDataFH();
    if (dataFH->isInMemoryMode() || dataFH->getNumPages() == 0) {
        return dataFH->isInMemoryMode();
    }
    auto* fileInfo = dataFH->getFileInfo();
    auto header = DatabaseHeader::readDatabaseHeader(*fileInfo);
    if (!header.has_value()) {
        return false;
    }
    sm.setDatabaseHeader(std::make_unique<DatabaseHeader>(*header));
    auto* pageManager = dataFH->getPageManager();
    if (header->metadataPageRange.startPageIdx != INVALID_PAGE_IDX) {
        auto reader = std::make_unique<common::BufferedFileReader>(*fileInfo);
        reader->resetReadOffset(header->metadataPageRange.startPageIdx * common::LBUG_PAGE_SIZE);
        common::Deserializer deSer(std::move(reader));
        pageManager->deserialize(deSer);
    }
    if (header->dataFileNumPages != 0) {
        pageManager->reclaimTailPagesIfNeeded(header->dataFileNumPages);
    }
    return true;
}

StorageManager& PartitionStorageRegistry::getOrCreate(main::ClientContext* context,
    table_id_t tableID, const std::string& childName, bool requireExistingFile,
    bool loadChildState) {
    {
        std::shared_lock slock{mtx};
        if (auto it = managers.find(tableID); it != managers.end()) {
            return *it->second;
        }
    }
    std::unique_lock xlock{mtx};
    if (auto it = managers.find(tableID); it != managers.end()) {
        return *it->second;
    }
    auto path = getChildPath(context, childName);
    auto* vfs = VirtualFileSystem::GetUnsafe(*context);
    if (requireExistingFile && !vfs->fileOrPathExists(path, context)) {
        throw RuntimeException(std::format(
            "Cannot recover committed checkpoint: partition file {} is missing.", path));
    }
    auto storageManager = std::make_unique<StorageManager>(path, context->getDBConfig()->readOnly,
        false /* enableChecksums */, *MemoryManager::Get(*context), false /* enableCompression */,
        context->getDBConfig()->enableDefaultHashIndex, vfs);
    storageManager->initDataFileHandle(vfs, context);
    if (loadChildState && !loadChildHeaderAndPageManager(*storageManager) && requireExistingFile) {
        throw RuntimeException(std::format(
            "Cannot recover committed checkpoint: partition file {} has no valid header.", path));
    }
    auto* raw = storageManager.get();
    managers.emplace(tableID, std::move(storageManager));
    return *raw;
}

StorageManager* PartitionStorageRegistry::tryGet(table_id_t tableID) {
    std::shared_lock slock{mtx};
    auto it = managers.find(tableID);
    return it == managers.end() ? nullptr : it->second.get();
}

bool PartitionStorageRegistry::isRemotelyRouted(table_id_t parentTableID, uint64_t partitionIndex) {
    const auto* hooks = common::getPartitionRoutingHooks();
    if (hooks == nullptr || hooks->locate == nullptr) {
        return false;
    }
    common::PartitionHandle handle = nullptr;
    return hooks->locate(hooks->context, common::PartitionRef{parentTableID, partitionIndex},
        &handle);
}

NodeTable* PartitionStorageRegistry::resolveNodeTable(main::ClientContext* context,
    TableCatalogEntry& entry) {
    return resolveNodeTable(context, entry, nullptr /* ambient */);
}

NodeTable* PartitionStorageRegistry::resolveNodeTable(main::ClientContext* context,
    TableCatalogEntry& entry, catalog::Catalog* ownerCatalog) {
    auto* mainSM = ownerCatalog != nullptr ? resolveOwnerStorageManager(context, ownerCatalog) :
                                             StorageManager::Get(*context);
    const auto tableID = entry.getTableID();
    if (entry.getType() != CatalogEntryType::NODE_TABLE_ENTRY ||
        !entry.ptrCast<NodeTableCatalogEntry>()->isPartitionChild()) {
        return mainSM->getTable(tableID)->ptrCast<NodeTable>();
    }
    // Partition children live in their own data files (see phase-B design in
    // docs/partitioning.md). After a reopen the registry is empty: lazily re-open the child's
    // file from its catalog entry name on first touch. Children claimed by a routing wrapper
    // own no local state and are served through the hooks instead.
    const auto* nodeEntry = entry.ptrCast<NodeTableCatalogEntry>();
    if (isRemotelyRouted(nodeEntry->getParentTableID(), nodeEntry->getPartitionIndex())) {
        throw RuntimeException(std::format("Partition subgraph {} is routed remotely; its data "
                                           "is not stored locally.",
            entry.getName()));
    }
    auto& registry = *Get(context);
    if (auto* sm = registry.tryGet(tableID)) {
        return sm->getTable(tableID)->ptrCast<NodeTable>();
    }
    auto& sm = registry.getOrCreate(context, tableID, entry.getName());
    if (!sm.containsTable(tableID)) {
        sm.createTable(const_cast<TableCatalogEntry*>(&entry), context);
    }
    return sm.getTable(tableID)->ptrCast<NodeTable>();
}

NodeTable* PartitionStorageRegistry::resolveNodeTableByID(main::ClientContext* context,
    table_id_t tableID) {
    auto* mainSM = StorageManager::Get(*context);
    if (mainSM->containsTable(tableID)) {
        return mainSM->getTable(tableID)->ptrCast<NodeTable>();
    }
    auto* entry = Catalog::Get(*context)->getTableCatalogEntry(
        transaction::Transaction::Get(*context), tableID);
    return resolveNodeTable(context, *entry);
}

NodeTable* PartitionStorageRegistry::resolveNodeTableByID(main::ClientContext* context,
    table_id_t tableID, catalog::Catalog* ownerCatalog) {
    auto* ownerSM = resolveOwnerStorageManager(context, ownerCatalog);
    if (ownerSM->containsTable(tableID)) {
        return ownerSM->getTable(tableID)->ptrCast<NodeTable>();
    }
    auto* entry =
        ownerCatalog->getTableCatalogEntry(transaction::Transaction::Get(*context), tableID);
    return resolveNodeTable(context, *entry, ownerCatalog);
}

StorageManager* PartitionStorageRegistry::resolveOwnerStorageManager(main::ClientContext* context,
    catalog::Catalog* ownerCatalog) {
    if (auto* sm = ownerCatalog->getStorageManager()) {
        return sm;
    }
    return context->getDatabase()->getStorageManager();
}

std::vector<StorageManager*> PartitionStorageRegistry::getAllManagers() {
    std::shared_lock lck{mtx};
    std::vector<StorageManager*> out;
    out.reserve(managers.size());
    for (auto& [_, sm] : managers) {
        out.push_back(sm.get());
    }
    return out;
}

void PartitionStorageRegistry::openAllChildren(main::ClientContext* context,
    const catalog::Catalog& catalog, bool recoveringCommittedCheckpoint) {
    const auto* txn = &transaction::DUMMY_CHECKPOINT_TRANSACTION;
    for (auto* entry : catalog.getNodeTableEntries(txn)) {
        auto* nodeEntry = entry->ptrCast<NodeTableCatalogEntry>();
        if (!nodeEntry->isPartitionChild()) {
            continue;
        }
        // Children claimed by a routing wrapper own no local state.
        if (isRemotelyRouted(nodeEntry->getParentTableID(), nodeEntry->getPartitionIndex())) {
            continue;
        }
        const auto tableID = nodeEntry->getTableID();
        if (recoveringCommittedCheckpoint) {
            // An interrupted shadow-page apply can leave a child's header or page-manager pages
            // half-replaced, so the file cannot be parsed yet. Register it and repair first
            // (replayCheckpointShadows), which loads the header and page manager afterwards —
            // the same repair-then-parse order the main database file already follows. Tables
            // are constructed later from the main file's metadata stream
            // (StorageManager::deserialize).
            static_cast<void>(getOrCreate(context, tableID, nodeEntry->getName(),
                /*requireExistingFile=*/true, /*loadChildState=*/false));
            continue;
        }
        auto& sm = getOrCreate(context, tableID, nodeEntry->getName(), false);
        if (!sm.containsTable(tableID)) {
            sm.createTable(nodeEntry, context);
        }
    }
}

void PartitionStorageRegistry::replayCheckpointShadows(main::ClientContext* context,
    std::optional<uuid> legacyDatabaseID, bool checkpointBundle) {
    auto* vfs = VirtualFileSystem::GetUnsafe(*context);
    for (auto* sm : getAllManagers()) {
        const auto shadowPath = StorageUtils::getShadowFilePath(sm->getDatabasePath());
        if (!vfs->fileOrPathExists(shadowPath, context)) {
            if (checkpointBundle) {
                throw RuntimeException(std::format(
                    "Cannot recover committed checkpoint: partition shadow file {} is missing.",
                    shadowPath));
            }
            // The legacy protocol applied and removed each child's shadow before the checkpoint
            // committed, so a missing shadow means the child file is already fully applied.
            continue;
        }
        ShadowFile::replayShadowPageRecordsForStorageManager(*context, *sm, legacyDatabaseID);
    }
    // After the replay the files hold a consistent view for the first time in this open: load
    // the header and page manager now, before anything allocates from them. Children without
    // shadows (already applied by the legacy protocol) load straight from their files.
    for (auto* sm : getAllManagers()) {
        if (!loadChildHeaderAndPageManager(*sm)) {
            throw RuntimeException(std::format(
                "Cannot recover committed checkpoint: partition file {} has no valid header "
                "after replaying its shadow.",
                sm->getDatabasePath()));
        }
    }
}

void PartitionStorageRegistry::dropAll(main::ClientContext* context,
    const std::vector<table_id_t>& tableIDs) {
    std::vector<std::string> paths;
    {
        std::unique_lock xlock{mtx};
        for (auto tableID : tableIDs) {
            auto it = managers.find(tableID);
            if (it == managers.end()) {
                continue;
            }
            paths.push_back(it->second->getDatabasePath());
            it->second->closeFileHandle();
            managers.erase(it);
        }
    }
    auto vfs = VirtualFileSystem::GetUnsafe(*context);
    for (const auto& path : paths) {
        vfs->removeFileIfExists(path, context);
        vfs->removeFileIfExists(StorageUtils::getWALFilePath(path), context);
        vfs->removeFileIfExists(StorageUtils::getShadowFilePath(path), context);
    }
}

void PartitionStorageRegistry::dropAllNotInCatalog(main::ClientContext* context,
    const catalog::Catalog& catalog) {
    std::vector<table_id_t> dropped;
    {
        std::shared_lock lck{mtx};
        const auto* txn = &transaction::DUMMY_CHECKPOINT_TRANSACTION;
        for (const auto& [tableID, sm] : managers) {
            if (!catalog.containsTable(txn, tableID, true)) {
                dropped.push_back(tableID);
            }
        }
    }
    if (!dropped.empty()) {
        dropAll(context, dropped);
    }
}

void PartitionStorageRegistry::renameChild(main::ClientContext* context, table_id_t tableID,
    const std::string& newChildName) {
    StorageManager* sm = nullptr;
    std::string oldPath;
    {
        std::unique_lock xlock{mtx};
        auto it = managers.find(tableID);
        if (it == managers.end()) {
            return;
        }
        sm = it->second.get();
        oldPath = sm->getDatabasePath();
    }
    namespace fs = std::filesystem;
    const auto dir = fs::path(oldPath).parent_path();
    const auto newPath = (dir / fs::path(getChildPath(context, newChildName)).filename()).string();
    auto vfs = VirtualFileSystem::GetUnsafe(*context);
    sm->closeFileHandle();
    const std::pair<std::string, std::string> moves[] = {{oldPath, newPath},
        {StorageUtils::getWALFilePath(oldPath), StorageUtils::getWALFilePath(newPath)},
        {StorageUtils::getShadowFilePath(oldPath), StorageUtils::getShadowFilePath(newPath)}};
    for (const auto& [from, to] : moves) {
        if (vfs->fileOrPathExists(from, context)) {
            vfs->renameFile(from, to);
        }
    }
    sm->setDatabasePath(newPath);
    sm->initDataFileHandle(vfs, context);
}

void PartitionStorageRegistry::reconcilePaths(main::ClientContext* context,
    const catalog::Catalog& catalog) {
    std::vector<std::pair<table_id_t, std::string>> renames;
    {
        std::shared_lock lck{mtx};
        const auto* txn = &transaction::DUMMY_CHECKPOINT_TRANSACTION;
        // Iterate node-table entries directly: per-ID lookups can miss entries committed after
        // the last checkpoint, while entry iteration reflects the live catalog.
        std::unordered_map<table_id_t, std::string> expectedNames;
        for (auto* entry : catalog.getNodeTableEntries(txn)) {
            auto* nodeEntry = entry->ptrCast<NodeTableCatalogEntry>();
            if (nodeEntry->isPartitionChild()) {
                expectedNames.emplace(nodeEntry->getTableID(), nodeEntry->getName());
            }
        }
        for (const auto& [tableID, sm] : managers) {
            auto it = expectedNames.find(tableID);
            if (it == expectedNames.end()) {
                continue;
            }
            const auto expected =
                std::filesystem::path(getChildPath(context, it->second)).filename().string();
            if (std::filesystem::path(sm->getDatabasePath()).filename().string() != expected) {
                renames.emplace_back(tableID, it->second);
            }
        }
    }
    for (const auto& [tableID, newName] : renames) {
        renameChild(context, tableID, newName);
    }
}

} // namespace storage
} // namespace lbug

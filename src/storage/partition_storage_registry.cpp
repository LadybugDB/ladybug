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
#include "main/database_manager.h"
#include "main/db_config.h"
#include "storage/database_header.h"
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
static void loadChildHeaderAndPageManager(StorageManager& sm) {
    auto* dataFH = sm.getDataFH();
    if (dataFH->isInMemoryMode() || dataFH->getNumPages() == 0) {
        return;
    }
    auto* fileInfo = dataFH->getFileInfo();
    auto header = DatabaseHeader::readDatabaseHeader(*fileInfo);
    if (!header.has_value()) {
        return;
    }
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
}

StorageManager& PartitionStorageRegistry::getOrCreate(main::ClientContext* context,
    table_id_t tableID, const std::string& childName) {
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
    auto storageManager = std::make_unique<StorageManager>(path, false /* readOnly */,
        false /* enableChecksums */, *MemoryManager::Get(*context), false /* enableCompression */,
        context->getDBConfig()->enableDefaultHashIndex, VirtualFileSystem::GetUnsafe(*context));
    storageManager->initDataFileHandle(VirtualFileSystem::GetUnsafe(*context), context);
    loadChildHeaderAndPageManager(*storageManager);
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
    auto* mainSM = StorageManager::Get(*context);
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
    const catalog::Catalog& catalog) {
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
        auto& sm = getOrCreate(context, tableID, nodeEntry->getName());
        if (!sm.containsTable(tableID)) {
            sm.createTable(nodeEntry, context);
        }
    }
}

void PartitionStorageRegistry::reloadPageManagers() {
    std::shared_lock slock{mtx};
    for (auto& [_, sm] : managers) {
        loadChildHeaderAndPageManager(*sm);
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

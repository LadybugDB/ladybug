#include "storage/local_storage/local_storage.h"

#include "catalog/catalog.h"
#include "storage/local_storage/local_node_table.h"
#include "storage/local_storage/local_rel_table.h"
#include "storage/local_storage/local_table.h"
#include "storage/partition_storage_registry.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "storage/table/rel_table.h"
#include "storage/table/table.h"

using namespace lbug::common;
using namespace lbug::transaction;

namespace lbug {
namespace storage {

LocalTable* LocalStorage::getOrCreateLocalTable(Table& table) {
    const auto tableID = table.getTableID();
    auto catalog = catalog::Catalog::Get(clientContext);
    auto transaction = transaction::Transaction::Get(clientContext);
    auto& mm = *MemoryManager::Get(clientContext);
    if (!tables.contains(tableID)) {
        switch (table.getTableType()) {
        case TableType::NODE: {
            auto tableEntry = catalog->getTableCatalogEntry(transaction, table.getTableID());
            tables[tableID] = std::make_unique<LocalNodeTable>(tableEntry, table, mm);
        } break;
        case TableType::REL: {
            // We have to fetch the rel group entry from the catalog to based on the relGroupID.
            auto tableEntry =
                catalog->getTableCatalogEntry(transaction, table.cast<RelTable>().getRelGroupID());
            tables[tableID] = std::make_unique<LocalRelTable>(tableEntry, table, mm);
        } break;
        default:
            UNREACHABLE_CODE;
        }
    }
    return tables.at(tableID).get();
}

LocalTable* LocalStorage::getLocalTable(table_id_t tableID) const {
    if (tables.contains(tableID)) {
        return tables.at(tableID).get();
    }
    return nullptr;
}

PageAllocator* LocalStorage::addOptimisticAllocator(StorageManager* sm) {
    auto* effectiveSM = sm != nullptr ? sm : StorageManager::Get(clientContext);
    auto* dataFH = effectiveSM->getDataFH();
    if (dataFH->isInMemoryMode()) {
        return dataFH->getPageManager();
    }
    UniqLock lck{mtx};
    if (const auto it = allocatorsByStorageManager.find(effectiveSM);
        it != allocatorsByStorageManager.end()) {
        return it->second;
    }
    optimisticAllocators.emplace_back(
        std::make_unique<OptimisticAllocator>(*dataFH->getPageManager()));
    auto* allocator = optimisticAllocators.back().get();
    allocatorsByStorageManager[effectiveSM] = allocator;
    return allocator;
}

void LocalStorage::commit() {
    auto catalog = catalog::Catalog::Get(clientContext);
    auto transaction = transaction::Transaction::Get(clientContext);
    auto storageManager = StorageManager::Get(clientContext);
    for (auto& [tableID, localTable] : tables) {
        if (localTable->getTableType() == TableType::NODE) {
            const auto tableEntry = catalog->getTableCatalogEntry(transaction, tableID);
            const auto table =
                storage::PartitionStorageRegistry::resolveNodeTableByID(&clientContext, tableID);
            table->commit(&clientContext, tableEntry, localTable.get());
        }
    }
    for (auto& [tableID, localTable] : tables) {
        if (localTable->getTableType() == TableType::REL) {
            const auto table = storageManager->getTable(tableID);
            const auto tableEntry =
                catalog->getTableCatalogEntry(transaction, table->cast<RelTable>().getRelGroupID());
            table->commit(&clientContext, tableEntry, localTable.get());
        }
    }
    for (auto& optimisticAllocator : optimisticAllocators) {
        optimisticAllocator->commit();
    }
}

void LocalStorage::rollback() {
    auto mm = MemoryManager::Get(clientContext);
    for (auto& [_, localTable] : tables) {
        localTable->clear(*mm);
    }
    for (auto& optimisticAllocator : optimisticAllocators) {
        optimisticAllocator->rollback();
    }
    auto& pageManager = *PageManager::Get(clientContext);
    pageManager.mergeFreePages(pageManager.getDataFH());
    pageManager.clearEvictedBMEntriesIfNeeded(mm->getBufferManager());
}

} // namespace storage
} // namespace lbug

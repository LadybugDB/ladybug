#include "storage/local_storage/local_storage.h"

#include "catalog/catalog.h"
#include "main/client_context.h"
#include "main/database.h"
#include "main/database_manager.h"
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

namespace {
catalog::Catalog* getMainCatalog(main::ClientContext& clientContext) {
    return clientContext.getDatabase()->getCatalog();
}

std::unique_ptr<LocalTable> makeLocalTable(catalog::Catalog* ownerCatalog,
    transaction::Transaction* transaction, const LocalTableKey& key, Table& table,
    MemoryManager& mm) {
    switch (table.getTableType()) {
    case TableType::NODE: {
        auto tableEntry = ownerCatalog->getTableCatalogEntry(transaction, key.tableID);
        return std::make_unique<LocalNodeTable>(tableEntry, table, mm);
    } break;
    case TableType::REL: {
        // We have to fetch the rel group entry from the catalog to based on the relGroupID.
        auto tableEntry =
            ownerCatalog->getTableCatalogEntry(transaction, table.cast<RelTable>().getRelGroupID());
        return std::make_unique<LocalRelTable>(tableEntry, table, mm);
    } break;
    default:
        UNREACHABLE_CODE;
    }
}
} // namespace

LocalTable* LocalStorage::getOrCreateLocalTable(Table& table) {
    const auto tableID = table.getTableID();
    const auto key = LocalTableKey{table.getOwnerCatalogName(), tableID};
    auto transaction = transaction::Transaction::Get(clientContext);
    auto& mm = *MemoryManager::Get(clientContext);
    if (!tables.contains(key)) {
        // The owner catalog must stay alive while the local table reads its entry, so a
        // graph owner is resolved under the registry's shared lock (withGraphCatalog) —
        // a concurrent DROP GRAPH can destroy the catalog the moment a plain
        // getGraphCatalog lookup releases it.
        if (key.ownerCatalogName.empty()) {
            tables[key] =
                makeLocalTable(getMainCatalog(clientContext), transaction, key, table, mm);
        } else {
            main::DatabaseManager::Get(clientContext)
                ->withGraphCatalog(key.ownerCatalogName, [&](catalog::Catalog* ownerCatalog) {
                    tables[key] = makeLocalTable(ownerCatalog, transaction, key, table, mm);
                });
        }
    }
    return tables.at(key).get();
}

LocalTable* LocalStorage::getLocalTable(const Table& table) const {
    const auto key = LocalTableKey{table.getOwnerCatalogName(), table.getTableID()};
    if (tables.contains(key)) {
        return tables.at(key).get();
    }
    return nullptr;
}

LocalTable* LocalStorage::getLocalTable(common::table_id_t tableID) const {
    LocalTable* result = nullptr;
    for (const auto& [key, table] : tables) {
        if (key.tableID != tableID) {
            continue;
        }
        if (result != nullptr) {
            return nullptr;
        }
        result = table.get();
    }
    return result;
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
    auto transaction = transaction::Transaction::Get(clientContext);
    for (auto& [key, localTable] : tables) {
        if (localTable->getTableType() == TableType::NODE) {
            // Catalog read, table resolution, and the commit below must run under one
            // hold of the graph registry's shared lock: a concurrent DROP GRAPH must not
            // destroy the owner catalog while staged graph data is being committed.
            const auto commitInto = [&](catalog::Catalog* ownerCatalog) {
                const auto tableEntry =
                    ownerCatalog->getTableCatalogEntry(transaction, key.tableID);
                const auto table = storage::PartitionStorageRegistry::resolveNodeTableByID(
                    &clientContext, key.tableID, ownerCatalog);
                table->commit(&clientContext, tableEntry, localTable.get());
            };
            if (key.ownerCatalogName.empty()) {
                commitInto(getMainCatalog(clientContext));
            } else {
                main::DatabaseManager::Get(clientContext)
                    ->withGraphCatalog(key.ownerCatalogName, commitInto);
            }
        }
    }
    for (auto& [key, localTable] : tables) {
        if (localTable->getTableType() == TableType::REL) {
            const auto commitInto = [&](catalog::Catalog* ownerCatalog) {
                const auto table = PartitionStorageRegistry::resolveOwnerStorageManager(
                    &clientContext, ownerCatalog)
                                       ->getTable(key.tableID);
                const auto tableEntry = ownerCatalog->getTableCatalogEntry(transaction,
                    table->cast<RelTable>().getRelGroupID());
                table->commit(&clientContext, tableEntry, localTable.get());
            };
            if (key.ownerCatalogName.empty()) {
                commitInto(getMainCatalog(clientContext));
            } else {
                main::DatabaseManager::Get(clientContext)
                    ->withGraphCatalog(key.ownerCatalogName, commitInto);
            }
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

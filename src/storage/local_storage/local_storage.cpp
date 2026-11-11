#include "storage/local_storage/local_storage.h"

#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/exception/catalog.h"
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
#include "transaction/transaction_manager.h"
#include <format>

using namespace lbug::common;
using namespace lbug::transaction;

namespace lbug {
namespace storage {

namespace {
catalog::Catalog* getMainCatalog(main::ClientContext& clientContext) {
    return clientContext.getDatabase()->getCatalog();
}

// Staged rows are appended to storage by column ID, so two versions of a table have the same
// columns when the IDs and types match. A rename or a comment keeps them. ADD and DROP change
// them, and so does DROP followed by ADD of the same name, which keeps the column count.
bool hasSameColumns(const catalog::TableCatalogEntry& lhs, const catalog::TableCatalogEntry& rhs) {
    // getProperties(), not getProperty(i): property IDs skip dropped properties, so positional
    // getProperty(i) throws after a DROP.
    const auto lhsProps = lhs.getProperties();
    const auto rhsProps = rhs.getProperties();
    if (lhsProps.size() != rhsProps.size()) {
        return false;
    }
    for (auto i = 0u; i < lhsProps.size(); i++) {
        if (lhs.getColumnID(lhsProps[i].getName()) != rhs.getColumnID(rhsProps[i].getName()) ||
            lhsProps[i].getType() != rhsProps[i].getType()) {
            return false;
        }
    }
    return true;
}

// The newest catalog version of the table, committed or not, decides the columns its storage
// has; `tableEntry` is the version this transaction sees.
void throwIfColumnsChanged(const catalog::Catalog& ownerCatalog,
    const catalog::TableCatalogEntry& tableEntry, const std::string& tableName) {
    bool dropped = false;
    bool columnsChanged = false;
    ownerCatalog.readLatestTableEntry(tableEntry.getTableID(), [&](const auto* head) {
        const auto* headTable = (head != nullptr && !head->isDeleted()) ?
                                    dynamic_cast<const catalog::TableCatalogEntry*>(head) :
                                    nullptr;
        dropped = headTable == nullptr;
        columnsChanged = !dropped && !hasSameColumns(tableEntry, *headTable);
    });
    if (dropped) {
        throw common::CatalogException{std::format(
            "Write-write conflict on inserting into table {}: it was dropped by another "
            "transaction.",
            tableName)};
    }
    if (columnsChanged) {
        throw common::CatalogException{
            std::format("Write-write conflict on inserting into table {}: its columns were "
                        "changed by another transaction.",
                tableName)};
    }
}

// Also registers `writer` as a writer of the table; see TransactionManager::addLocalWriter.
std::unique_ptr<LocalTable> makeLocalTable(catalog::Catalog* ownerCatalog,
    transaction::Transaction* transaction, const LocalTableKey& key, Table& table,
    MemoryManager& mm, transaction::TransactionManager& transactionManager,
    const LocalStorage* writer) {
    std::unique_ptr<LocalTable> localTable;
    catalog::TableCatalogEntry* tableEntry = nullptr;
    std::string tableName;
    switch (table.getTableType()) {
    case TableType::NODE: {
        tableEntry = ownerCatalog->getTableCatalogEntry(transaction, table.getTableID());
        localTable = std::make_unique<LocalNodeTable>(tableEntry, table, mm);
        // A partition stages the inserts made into its partitioned table, which the user named.
        const auto* nodeEntry = tableEntry->ptrCast<catalog::NodeTableCatalogEntry>();
        tableName =
            nodeEntry->isPartitionChild() ?
                ownerCatalog->getTableCatalogEntry(transaction, nodeEntry->getParentTableID())
                    ->getName() :
                tableEntry->getName();
    } break;
    case TableType::REL: {
        // A rel table's catalog entry is its rel group's, not its own table ID's.
        tableEntry =
            ownerCatalog->getTableCatalogEntry(transaction, table.cast<RelTable>().getRelGroupID());
        localTable = std::make_unique<LocalRelTable>(tableEntry, table, mm);
        tableName = tableEntry->getName();
    } break;
    default:
        UNREACHABLE_CODE;
    }
    transactionManager.addLocalWriter(key.ownerCatalogName, key.tableID, writer,
        [&] { throwIfColumnsChanged(*ownerCatalog, *tableEntry, tableName); });
    return localTable;
}
} // namespace

LocalStorage::~LocalStorage() {
    for (const auto& [key, _] : tables) {
        transactionManager->removeLocalWriter(key.ownerCatalogName, key.tableID, this);
    }
}

LocalTable* LocalStorage::getOrCreateLocalTable(Table& table) {
    const auto tableID = table.getTableID();
    const auto key = LocalTableKey{table.getOwnerCatalogName(), tableID};
    auto transaction = transaction::Transaction::Get(clientContext);
    auto& mm = *MemoryManager::Get(clientContext);
    if (!tables.contains(key)) {
        transactionManager = transaction::TransactionManager::Get(clientContext);
        // The owner catalog must stay alive while the local table reads its entry, so a
        // graph owner is resolved under the registry's shared lock (withGraphCatalog) —
        // a concurrent DROP GRAPH can destroy the catalog the moment a plain
        // getGraphCatalog lookup releases it.
        if (key.ownerCatalogName.empty()) {
            tables[key] = makeLocalTable(getMainCatalog(clientContext), transaction, key, table, mm,
                *transactionManager, this);
        } else {
            main::DatabaseManager::Get(clientContext)
                ->withGraphCatalog(key.ownerCatalogName, [&](catalog::Catalog* ownerCatalog) {
                    tables[key] = makeLocalTable(ownerCatalog, transaction, key, table, mm,
                        *transactionManager, this);
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

void LocalStorage::retainOptimisticAllocations(StorageManager* sm, const NodeGroup& nodeGroup) {
    auto* effectiveSM = sm != nullptr ? sm : StorageManager::Get(clientContext);
    const auto it = allocatorsByStorageManager.find(effectiveSM);
    if (it == allocatorsByStorageManager.end()) {
        return;
    }
    // Reuse the column hierarchy's page traversal without freeing the retained pages.
    struct PageRetainer final : PageAllocator {
        explicit PageRetainer(OptimisticAllocator& allocator)
            : PageAllocator{allocator.getDataFH()}, allocator{allocator} {}
        PageRange allocatePageRange(common::page_idx_t) override { UNREACHABLE_CODE; }
        void freePageRange(PageRange block) override { allocator.retainPageRange(block); }
        OptimisticAllocator& allocator;
    } retainer{*it->second};
    nodeGroup.reclaimStorage(retainer);
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

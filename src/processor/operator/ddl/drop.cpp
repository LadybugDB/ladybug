#include "processor/operator/ddl/drop.h"

#include "catalog/catalog.h"
#include "catalog/catalog_entry/index_catalog_entry.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/binder.h"
#include "common/string_utils.h"
#include "main/client_context.h"
#include "main/database.h"
#include "main/database_manager.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "transaction/transaction.h"
#include <format>

using namespace lbug::catalog;
using namespace lbug::common;

namespace lbug {
namespace processor {

void Drop::executeInternal(ExecutionContext* context) {
    auto clientContext = context->clientContext;
    switch (dropInfo.dropType) {
    case DropType::SEQUENCE: {
        dropSequence(clientContext);
    } break;
    case DropType::TABLE: {
        dropTable(clientContext);
    } break;
    case DropType::MACRO: {
        dropMacro(clientContext);
    } break;
    case DropType::GRAPH: {
        dropGraph(clientContext);
    } break;
    case DropType::INDEX: {
        dropIndex(clientContext);
    } break;
    default:
        UNREACHABLE_CODE;
    }
}

void Drop::dropSequence(const main::ClientContext* context) {
    auto catalog = Catalog::Get(*context);
    auto transaction = transaction::Transaction::Get(*context);
    auto memoryManager = storage::MemoryManager::Get(*context);
    if (!catalog->containsSequence(transaction, dropInfo.name)) {
        auto message = std::format("Sequence {} does not exist.", dropInfo.name);
        switch (dropInfo.conflictAction) {
        case ConflictAction::ON_CONFLICT_DO_NOTHING: {
            appendMessage(message, memoryManager);
            return;
        }
        case ConflictAction::ON_CONFLICT_THROW: {
            throw BinderException(message);
        }
        default:
            UNREACHABLE_CODE;
        }
    }
    catalog->dropSequence(transaction, dropInfo.name);
    appendMessage(std::format("Sequence {} has been dropped.", dropInfo.name), memoryManager);
}

void Drop::dropTable(const main::ClientContext* context) {
    auto catalog = Catalog::Get(*context);
    auto transaction = transaction::Transaction::Get(*context);
    auto memoryManager = storage::MemoryManager::Get(*context);
    if (!catalog->containsTable(transaction, dropInfo.name, context->useInternalCatalogEntry())) {
        auto message = std::format("Table {} does not exist.", dropInfo.name);
        switch (dropInfo.conflictAction) {
        case ConflictAction::ON_CONFLICT_DO_NOTHING: {
            appendMessage(message, memoryManager);
            return;
        }
        case ConflictAction::ON_CONFLICT_THROW: {
            throw BinderException(message);
        }
        default:
            UNREACHABLE_CODE;
        }
    }
    auto entry = catalog->getTableCatalogEntry(transaction, dropInfo.name);
    switch (entry->getType()) {
    case CatalogEntryType::NODE_TABLE_ENTRY: {
        auto* nodeEntry = entry->ptrCast<NodeTableCatalogEntry>();
        if (nodeEntry->isPartitionChild()) {
            // A partition subgraph is owned by its partitioned parent and is dropped with it.
            // Dropping it individually would leave the parent pointing at a missing table.
            auto* parent =
                catalog->getTableCatalogEntry(transaction, nodeEntry->getParentTableID());
            throw BinderException(std::format(
                "Cannot drop table {} because it is a partition of partitioned table {}. "
                "Drop {} instead.",
                entry->getName(), parent->getName(), parent->getName()));
        }
        // A partitioned parent cascades to its partition subgraphs, so every table that this
        // DROP would remove must satisfy the reference checks below. This refuses the drop
        // atomically (before anything is dropped) when any partition is still referenced.
        std::vector<const NodeTableCatalogEntry*> droppingTables{nodeEntry};
        if (nodeEntry->isPartitioned()) {
            droppingTables.reserve(nodeEntry->getChildTableIDs().size() + 1);
            for (auto childID : nodeEntry->getChildTableIDs()) {
                droppingTables.push_back(catalog->getTableCatalogEntry(transaction, childID)
                                             ->ptrCast<NodeTableCatalogEntry>());
            }
        }
        for (auto* droppingEntry : droppingTables) {
            for (auto& indexEntry : catalog->getIndexEntries(transaction)) {
                if (indexEntry->getTableID() == droppingEntry->getTableID()) {
                    if (StringUtils::caseInsensitiveEquals(indexEntry->getIndexType(), "HASH") ||
                        StringUtils::caseInsensitiveEquals(indexEntry->getIndexType(), "ART")) {
                        continue;
                    }
                    throw BinderException(std::format(
                        "Cannot delete node table {} because it is referenced by index {}.",
                        droppingEntry->getName(), indexEntry->getIndexName()));
                }
            }
            for (auto& relEntry : catalog->getRelGroupEntries(transaction)) {
                if (relEntry->isParent(droppingEntry->getTableID())) {
                    throw BinderException(std::format("Cannot delete node table {} because it is "
                                                      "referenced by relationship table {}.",
                        droppingEntry->getName(), relEntry->getName()));
                }
            }
        }
    } break;
    case CatalogEntryType::REL_GROUP_ENTRY: {
        // Do nothing
    } break;
    default:
        UNREACHABLE_CODE;
    }
    catalog->dropTableEntryAndIndex(transaction, dropInfo.name);
    appendMessage(std::format("Table {} has been dropped.", dropInfo.name), memoryManager);
}

void Drop::dropMacro(const main::ClientContext* context) {
    auto catalog = Catalog::Get(*context);
    auto transaction = transaction::Transaction::Get(*context);
    auto memoryManager = storage::MemoryManager::Get(*context);
    handleMacroExistence(context);
    catalog->dropMacro(transaction, dropInfo.name);
    appendMessage(std::format("Macro {} has been dropped.", dropInfo.name), memoryManager);
}

void Drop::dropGraph(const main::ClientContext* context) {
    auto dbManager = main::DatabaseManager::Get(*context);
    auto memoryManager = storage::MemoryManager::Get(*context);

    if (StringUtils::caseInsensitiveEquals(dropInfo.name, "main")) {
        throw BinderException{"Cannot drop the main graph."};
    }

    // Every node table is registered as a subgraph (partition subgraphs included). Those
    // subgraphs are owned by their table: dropping one would desynchronize it from its table
    // (and, for a partition, from its partitioned parent), so refuse and point at the owner.
    const auto catalog = Catalog::Get(*context);
    const auto transaction = transaction::Transaction::Get(*context);
    if (catalog->containsGraph(transaction, dropInfo.name) &&
        catalog->containsTable(transaction, dropInfo.name, context->useInternalCatalogEntry())) {
        throw BinderException(std::format(
            "Cannot drop graph {}: it is a node-table subgraph. Drop the node table instead.",
            dropInfo.name));
    }

    if (!dbManager->hasGraph(dropInfo.name)) {
        auto message = std::format("Graph {} does not exist.", dropInfo.name);
        switch (dropInfo.conflictAction) {
        case ConflictAction::ON_CONFLICT_DO_NOTHING: {
            appendMessage(message, memoryManager);
            return;
        }
        case ConflictAction::ON_CONFLICT_THROW: {
            throw BinderException(message);
        }
        default:
            UNREACHABLE_CODE;
        }
    }

    if (dbManager->hasDefaultGraph() && StringUtils::getUpper(dbManager->getDefaultGraphName()) ==
                                            StringUtils::getUpper(dropInfo.name)) {
        dbManager->clearDefaultGraph();
    }

    dbManager->dropGraph(dropInfo.name, const_cast<main::ClientContext*>(context));
    appendMessage(std::format("Graph {} has been dropped.", dropInfo.name), memoryManager);
}

void Drop::dropIndex(const main::ClientContext* context) {
    auto catalog = Catalog::Get(*context);
    auto transaction = transaction::Transaction::Get(*context);
    auto memoryManager = storage::MemoryManager::Get(*context);
    if (!catalog->containsTable(transaction, dropInfo.name, context->useInternalCatalogEntry())) {
        auto message = std::format("Table {} does not exist.", dropInfo.name);
        switch (dropInfo.conflictAction) {
        case ConflictAction::ON_CONFLICT_DO_NOTHING: {
            appendMessage(message, memoryManager);
            return;
        }
        case ConflictAction::ON_CONFLICT_THROW: {
            throw BinderException(message);
        }
        default:
            UNREACHABLE_CODE;
        }
    }
    auto tableEntry = catalog->getTableCatalogEntry(transaction, dropInfo.name);
    if (tableEntry->getType() != CatalogEntryType::NODE_TABLE_ENTRY) {
        throw BinderException(
            std::format("Table {} is not a node table; cannot drop index.", dropInfo.name));
    }
    auto tableID = tableEntry->getTableID();
    auto storageManager = storage::StorageManager::Get(*context);
    auto* nodeTable = storageManager->getTable(tableID)->ptrCast<storage::NodeTable>();
    // An index may live in the catalog (user-created indexes), in storage only (the default
    // built-in PK hash index), or in both. Drop whichever are present.
    const auto inCatalog = catalog->containsIndex(transaction, tableID, dropInfo.indexName);
    const auto inStorage = nodeTable->getIndex(dropInfo.indexName).has_value();
    if (!inCatalog && !inStorage) {
        auto message =
            std::format("Index {} does not exist in table {}.", dropInfo.indexName, dropInfo.name);
        switch (dropInfo.conflictAction) {
        case ConflictAction::ON_CONFLICT_DO_NOTHING: {
            appendMessage(message, memoryManager);
            return;
        }
        case ConflictAction::ON_CONFLICT_THROW: {
            throw BinderException(message);
        }
        default:
            UNREACHABLE_CODE;
        }
    }
    if (inCatalog) {
        // Marks the IndexCatalogEntry deleted; the commit path emits the WAL drop record.
        catalog->dropIndex(transaction, tableID, dropInfo.indexName);
    }
    if (inStorage) {
        // Removes the in-memory index; its pages are reclaimed on the next checkpoint.
        nodeTable->dropIndex(dropInfo.indexName);
    }
    appendMessage(std::format("Index {} has been dropped.", dropInfo.indexName), memoryManager);
}

void Drop::handleMacroExistence(const main::ClientContext* context) {
    auto catalog = Catalog::Get(*context);
    auto transaction = transaction::Transaction::Get(*context);
    auto memoryManager = storage::MemoryManager::Get(*context);
    if (!catalog->containsMacro(transaction, dropInfo.name)) {
        auto message = std::format("Macro {} does not exist.", dropInfo.name);
        switch (dropInfo.conflictAction) {
        case ConflictAction::ON_CONFLICT_DO_NOTHING: {
            appendMessage(message, memoryManager);
            return;
        }
        case ConflictAction::ON_CONFLICT_THROW: {
            throw BinderException(message);
        }
        default:
            UNREACHABLE_CODE;
        }
    }
}

} // namespace processor
} // namespace lbug

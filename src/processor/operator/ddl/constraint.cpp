#include "processor/operator/ddl/constraint.h"

#include "catalog/catalog.h"
#include "catalog/catalog_entry/constraint_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/exception/binder.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/storage_manager.h"
#include "storage/unique_constraint_checker.h"
#include "transaction/transaction.h"
#include <format>

using namespace lbug::catalog;
using namespace lbug::common;

namespace lbug {
namespace processor {

void CreateConstraint::executeInternal(ExecutionContext* context) {
    auto clientContext = context->clientContext;
    auto catalog = Catalog::Get(*clientContext);
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto memoryManager = storage::MemoryManager::Get(*clientContext);
    if (catalog->containsConstraint(transaction, info.constraintName)) {
        switch (info.onConflict) {
        case ConflictAction::ON_CONFLICT_DO_NOTHING: {
            appendMessage(std::format("Constraint {} already exists.", info.constraintName),
                memoryManager);
            return;
        }
        case ConflictAction::ON_CONFLICT_THROW: {
            throw BinderException(
                std::format("Constraint {} already exists.", info.constraintName));
        }
        default:
            UNREACHABLE_CODE;
        }
    }
    auto tableEntry = catalog->getTableCatalogEntry(transaction, info.tableID);
    if (tableEntry == nullptr) {
        throw BinderException(std::format("Table {} does not exist.", info.tableName));
    }
    // Re-resolve property IDs by name: the bound IDs may predate same-transaction schema
    // changes (e.g. DROP + re-ADD COLUMN).
    std::vector<property_id_t> propertyIDs;
    std::vector<column_id_t> columnIDs;
    for (auto& propertyName : info.propertyNames) {
        if (!tableEntry->containsProperty(propertyName)) {
            throw BinderException(std::format("Table {} does not have property {}.",
                tableEntry->getName(), propertyName));
        }
        propertyIDs.push_back(tableEntry->getPropertyID(propertyName));
        columnIDs.push_back(tableEntry->getColumnID(propertyName));
    }
    if (info.isRelConstraint) {
        auto* groupEntry = tableEntry->ptrCast<RelGroupCatalogEntry>();
        storage::UniqueConstraintChecker::validateRelGroupNoDuplicates(clientContext, groupEntry,
            columnIDs, info.constraintName);
    } else {
        auto table = storage::StorageManager::Get(*clientContext)->getTable(info.tableID);
        storage::UniqueConstraintChecker::validateNoDuplicates(clientContext, table, columnIDs,
            info.constraintName);
    }
    auto entry = std::make_unique<ConstraintCatalogEntry>(info.constraintName, info.tableID,
        tableEntry->getName(), info.isRelConstraint, std::move(propertyIDs), info.propertyNames);
    catalog->createConstraint(transaction, std::move(entry));
    appendMessage(std::format("Constraint {} has been created.", info.constraintName),
        memoryManager);
}

void DropConstraint::executeInternal(ExecutionContext* context) {
    auto clientContext = context->clientContext;
    auto catalog = Catalog::Get(*clientContext);
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto memoryManager = storage::MemoryManager::Get(*clientContext);
    if (!catalog->containsConstraint(transaction, info.constraintName)) {
        switch (info.onConflict) {
        case ConflictAction::ON_CONFLICT_DO_NOTHING: {
            appendMessage(std::format("Constraint {} does not exist.", info.constraintName),
                memoryManager);
            return;
        }
        case ConflictAction::ON_CONFLICT_THROW: {
            throw BinderException(
                std::format("Constraint {} does not exist.", info.constraintName));
        }
        default:
            UNREACHABLE_CODE;
        }
    }
    catalog->dropConstraint(transaction, info.constraintName);
    appendMessage(std::format("Constraint {} has been dropped.", info.constraintName),
        memoryManager);
}

} // namespace processor
} // namespace lbug

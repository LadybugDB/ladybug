#include "processor/operator/ddl/constraint.h"

#include "catalog/catalog.h"
#include "catalog/catalog_entry/constraint_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/exception/binder.h"
#include "common/string_utils.h"
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
    // Re-resolve key parts by name: the bound IDs may predate same-transaction schema
    // changes (e.g. DROP + re-ADD COLUMN). Endpoint references (src/dst) resolve to
    // sentinels unless the table defines a real property with that name.
    std::vector<property_id_t> propertyIDs;
    for (auto& propertyName : info.propertyNames) {
        auto endpointID = property_id_t{INVALID_PROPERTY_ID};
        if (info.isRelConstraint) {
            if (StringUtils::caseInsensitiveEquals(propertyName, "src")) {
                endpointID = ConstraintCatalogEntry::SRC_ENDPOINT_ID;
            } else if (StringUtils::caseInsensitiveEquals(propertyName, "dst")) {
                endpointID = ConstraintCatalogEntry::DST_ENDPOINT_ID;
            }
        }
        if (endpointID != INVALID_PROPERTY_ID && !tableEntry->containsProperty(propertyName)) {
            propertyIDs.push_back(endpointID);
            continue;
        }
        if (!tableEntry->containsProperty(propertyName)) {
            throw BinderException(std::format("Table {} does not have property {}.",
                tableEntry->getName(), propertyName));
        }
        propertyIDs.push_back(tableEntry->getPropertyID(propertyName));
    }
    const auto parts = storage::UniqueConstraintChecker::resolveParts(tableEntry, propertyIDs);
    if (info.isRelConstraint) {
        storage::UniqueConstraintChecker::validateRelGroupNoDuplicates(clientContext, tableEntry,
            parts, info.constraintName);
    } else {
        auto table = storage::StorageManager::Get(*clientContext)->getTable(info.tableID);
        storage::UniqueConstraintChecker::validateNoDuplicates(clientContext, table, parts,
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

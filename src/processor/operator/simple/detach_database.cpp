#include "processor/operator/simple/detach_database.h"

#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/string_utils.h"
#include "main/client_context.h"
#include "main/database.h"
#include "main/database_manager.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

namespace lbug {
namespace processor {

std::string DetatchDatabasePrintInfo::toString() const {
    return "Database: " + name;
}

void DetachDatabase::executeInternal(ExecutionContext* context) {
    auto clientContext = context->clientContext;
    auto dbManager = main::DatabaseManager::Get(*clientContext);
    // Attached foreign databases register shadow entries (node tables and
    // rel groups) in the main catalog under bare table names. Remove the
    // ones owned by the detached database: left behind, they shadow
    // same-named native tables and crash planning with a dangling database
    // reference.
    auto upperName = common::StringUtils::getUpper(dbName);
    auto* catalog = clientContext->getDatabase()->getCatalog();
    auto* transaction = transaction::Transaction::Get(*clientContext);
    bool useInternal = clientContext->useInternalCatalogEntry();
    std::vector<const catalog::TableCatalogEntry*> stale;
    for (auto* entry : catalog->getTableEntries(transaction, useInternal)) {
        std::string owner;
        if (auto* nodeEntry = dynamic_cast<catalog::NodeTableCatalogEntry*>(entry)) {
            owner = nodeEntry->getForeignDatabaseName();
        } else if (auto* relEntry = dynamic_cast<catalog::RelGroupCatalogEntry*>(entry)) {
            owner = relEntry->getForeignDatabaseName();
        }
        if (!owner.empty() && common::StringUtils::getUpper(owner) == upperName) {
            stale.push_back(entry);
        }
    }
    for (auto* entry : stale) {
        catalog->dropTableEntry(transaction, entry);
    }
    dbManager->detachDatabase(dbName);
    appendMessage("Detached database successfully.", storage::MemoryManager::Get(*clientContext));
}

} // namespace processor
} // namespace lbug

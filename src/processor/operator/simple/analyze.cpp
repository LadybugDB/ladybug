#include "processor/operator/simple/analyze.h"

#include "catalog/catalog.h"
#include "main/client_context.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/stats/planner_stats.h"
#include "storage/storage_manager.h"
#include "transaction/transaction.h"
#include <format>

namespace lbug {
namespace processor {

void Analyze::executeInternal(ExecutionContext* context) {
    auto* clientContext = context->clientContext;
    auto* storageManager = storage::StorageManager::Get(*clientContext);
    auto* catalog = catalog::Catalog::Get(*clientContext);
    auto* transaction = transaction::Transaction::Get(*clientContext);
    const auto analyzedTables =
        storage::analyzePlannerStats(*storageManager, *catalog, transaction, tableName);
    appendMessage(std::format("Analyzed {} table{}.", analyzedTables.size(),
                      analyzedTables.size() == 1 ? "" : "s"),
        storage::MemoryManager::Get(*clientContext));
}

std::string AnalyzePrintInfo::toString() const {
    std::string result = "Table: ";
    result += tableName.value_or("ALL");
    return result;
}

} // namespace processor
} // namespace lbug

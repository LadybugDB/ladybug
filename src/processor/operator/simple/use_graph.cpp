#include "processor/operator/simple/use_graph.h"

#include "main/client_context.h"
#include "main/database_manager.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"

namespace lbug {
namespace processor {

void UseGraph::executeInternal(ExecutionContext* context) {
    auto dbManager = main::DatabaseManager::Get(*context->clientContext);
    dbManager->setDefaultGraph(graphName);
    appendMessage("Used graph successfully.", storage::MemoryManager::Get(*context->clientContext));
}

void CreateGraph::executeInternal(ExecutionContext* context) {
    auto dbManager = main::DatabaseManager::Get(*context->clientContext);
    auto memoryManager = storage::MemoryManager::Get(*context->clientContext);
    dbManager->createGraph(graphName, memoryManager, context->clientContext);
    appendMessage("Created graph successfully.", memoryManager);
}

} // namespace processor
} // namespace lbug

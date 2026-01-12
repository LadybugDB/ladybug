#include "processor/operator/ddl/create_sequence.h"

#include "catalog/catalog.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"
#include <format>

using namespace lbug::catalog;
using namespace lbug::common;

namespace lbug {
namespace processor {

std::string CreateSequencePrintInfo::toString() const {
    return seqName;
}

void CreateSequence::executeInternal(ExecutionContext* context) {
    auto clientContext = context->clientContext;
    auto catalog = Catalog::Get(*clientContext);
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto memoryManager = storage::MemoryManager::Get(*clientContext);
    if (catalog->containsSequence(transaction, info.sequenceName)) {
        switch (info.onConflict) {
        case ConflictAction::ON_CONFLICT_DO_NOTHING: {
            appendMessage(std::format("Sequence {} already exists.", info.sequenceName),
                memoryManager);
            return;
        }
        default:
            break;
        }
    }
    catalog->createSequence(transaction, info);
    appendMessage(std::format("Sequence {} has been created.", info.sequenceName), memoryManager);
}

} // namespace processor
} // namespace lbug

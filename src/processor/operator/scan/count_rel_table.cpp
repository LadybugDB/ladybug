#include "processor/operator/scan/count_rel_table.h"

#include "processor/execution_context.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace processor {

void CountRelTable::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* /*context*/) {
    countVector = resultSet->getValueVector(countOutputPos).get();
    hasExecuted = false;
}

bool CountRelTable::getNextTuplesInternal(ExecutionContext* context) {
    if (hasExecuted) {
        return false;
    }
    hasExecuted = true;

    // Get the transaction to read committed + uncommitted data
    auto transaction = Transaction::Get(*context->clientContext);

    // Sum up the counts from all rel tables
    row_idx_t totalCount = 0;
    for (auto* relTable : relTables) {
        totalCount += relTable->getNumTotalRows(transaction);
    }

    // Write the count to the output vector
    countVector->state->getSelVectorUnsafe().setToUnfiltered(1);
    countVector->setValue<int64_t>(0, static_cast<int64_t>(totalCount));

    return true;
}

} // namespace processor
} // namespace lbug

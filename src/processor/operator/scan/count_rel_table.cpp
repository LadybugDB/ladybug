#include "processor/operator/scan/count_rel_table.h"

#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace processor {

void CountRelTable::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) {
    nodeIDVector = resultSet->getValueVector(nodeIDPos).get();
    countVector = resultSet->getValueVector(countOutputPos).get();
    hasExecuted = false;
    totalCount = 0;

    // Create a dedicated output state for rel table scanning.
    // This MUST be separate from nodeIDVector->state because:
    // 1. The child ScanNodeTable modifies nodeIDVector->state during its scan
    // 2. The rel table scan also needs to modify the output state's selection vector
    // Using the same state would cause conflicts and assertion failures.
    relScanOutState = std::make_shared<DataChunkState>();
    auto& mm = *MemoryManager::Get(*context->clientContext);
    scanState = std::make_unique<RelTableScanState>(mm, nodeIDVector, std::vector<ValueVector*>{},
        relScanOutState);
}

bool CountRelTable::getNextTuplesInternal(ExecutionContext* context) {
    if (hasExecuted) {
        return false;
    }

    auto transaction = Transaction::Get(*context->clientContext);

    // For each rel table, scan all bound nodes and count edges.
    // Follow the ScanRelTable pattern closely:
    // 1. setToTable once per table (sets up local table scan state for uncommitted data)
    // 2. Loop: scan until empty, then get next batch of bound nodes from child
    for (auto* relTable : relTables) {
        // Set up scan state - no columns needed since we're just counting
        std::vector<column_id_t> columnIDs;
        std::vector<ColumnPredicateSet> predicates;
        scanState->setToTable(transaction, relTable, std::move(columnIDs), std::move(predicates),
            direction);

        // Get first batch of bound nodes
        if (!children[0]->getNextTuple(context)) {
            continue;
        }
        relTable->initScanState(transaction, *scanState);

        while (true) {
            // Scan edges for current batch of bound nodes
            while (relTable->scan(transaction, *scanState)) {
                totalCount += scanState->outState->getSelVector().getSelSize();
            }

            // Get next batch of bound nodes from child
            if (!children[0]->getNextTuple(context)) {
                break;
            }
            relTable->initScanState(transaction, *scanState);
        }
    }

    hasExecuted = true;

    // Write the count to the output vector
    countVector->state->getSelVectorUnsafe().setToUnfiltered(1);
    countVector->setValue<int64_t>(0, static_cast<int64_t>(totalCount));

    return true;
}

} // namespace processor
} // namespace lbug

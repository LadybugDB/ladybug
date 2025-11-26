#include "processor/operator/scan/count_rel_table.h"

#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/local_storage/local_node_table.h"
#include "storage/local_storage/local_storage.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace processor {

void CountRelTable::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) {
    countVector = resultSet->getValueVector(countOutputPos).get();
    hasExecuted = false;
    totalCount = 0;

    auto& mm = *MemoryManager::Get(*context->clientContext);

    // Create internal node ID vector for scanning (not in ResultSet)
    internalNodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), &mm);
    internalNodeIDVector->state = std::make_shared<DataChunkState>();

    // Create node scan state - just scanning node IDs, no properties
    nodeScanState = std::make_unique<NodeTableScanState>(internalNodeIDVector.get(),
        std::vector<ValueVector*>{}, internalNodeIDVector->state);

    // Create rel scan state with dedicated output state
    relScanOutState = std::make_shared<DataChunkState>();
    relScanState = std::make_unique<RelTableScanState>(mm, internalNodeIDVector.get(),
        std::vector<ValueVector*>{}, relScanOutState);
}

bool CountRelTable::getNextTuplesInternal(ExecutionContext* context) {
    if (hasExecuted) {
        return false;
    }

    auto transaction = Transaction::Get(*context->clientContext);

    // For each node table, scan all nodes and count their edges in the rel tables
    for (auto* nodeTable : nodeTables) {
        // Initialize for scanning this table - just set up with no columns
        std::vector<column_id_t> nodeColumnIDs;
        std::vector<ColumnPredicateSet> nodePredicates;
        nodeScanState->setToTable(transaction, nodeTable, std::move(nodeColumnIDs),
            std::move(nodePredicates));

        // Scan committed node groups
        auto numNodeGroups = nodeTable->getNumCommittedNodeGroups();
        nodeScanState->source = TableScanSource::COMMITTED;

        for (node_group_idx_t nodeGroupIdx = 0; nodeGroupIdx < numNodeGroups; nodeGroupIdx++) {
            nodeScanState->nodeGroupIdx = nodeGroupIdx;
            nodeTable->initScanState(transaction, *nodeScanState);

            while (nodeTable->scan(transaction, *nodeScanState)) {
                // For each batch of nodes, scan all rel tables
                for (auto* relTable : relTables) {
                    std::vector<column_id_t> columnIDs;
                    std::vector<ColumnPredicateSet> predicates;
                    relScanState->setToTable(transaction, relTable, std::move(columnIDs),
                        std::move(predicates), direction);
                    relTable->initScanState(transaction, *relScanState);

                    while (relTable->scan(transaction, *relScanState)) {
                        totalCount += relScanState->outState->getSelVector().getSelSize();
                    }
                }
            }
        }

        // Also scan uncommitted node groups
        node_group_idx_t numUncommittedNodeGroups = 0;
        if (transaction->isWriteTransaction()) {
            if (const auto localTable =
                    transaction->getLocalStorage()->getLocalTable(nodeTable->getTableID())) {
                auto& localNodeTable = localTable->cast<LocalNodeTable>();
                numUncommittedNodeGroups = localNodeTable.getNumNodeGroups();
            }
        }
        nodeScanState->source = TableScanSource::UNCOMMITTED;

        for (node_group_idx_t nodeGroupIdx = 0; nodeGroupIdx < numUncommittedNodeGroups;
             nodeGroupIdx++) {
            nodeScanState->nodeGroupIdx = nodeGroupIdx;
            nodeTable->initScanState(transaction, *nodeScanState);

            while (nodeTable->scan(transaction, *nodeScanState)) {
                for (auto* relTable : relTables) {
                    std::vector<column_id_t> columnIDs;
                    std::vector<ColumnPredicateSet> predicates;
                    relScanState->setToTable(transaction, relTable, std::move(columnIDs),
                        std::move(predicates), direction);
                    relTable->initScanState(transaction, *relScanState);

                    while (relTable->scan(transaction, *relScanState)) {
                        totalCount += relScanState->outState->getSelVector().getSelSize();
                    }
                }
            }
        }
    }

    hasExecuted = true;

    // Write the count to the output vector (single value)
    countVector->state->getSelVectorUnsafe().setToUnfiltered(1);
    countVector->setValue<int64_t>(0, static_cast<int64_t>(totalCount));

    return true;
}

} // namespace processor
} // namespace lbug

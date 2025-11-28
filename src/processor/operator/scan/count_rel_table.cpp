#include "processor/operator/scan/count_rel_table.h"

#include "common/system_config.h"
#include "main/client_context.h"
#include "main/database.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/local_storage/local_rel_table.h"
#include "storage/local_storage/local_storage.h"
#include "storage/table/column.h"
#include "storage/table/column_chunk_data.h"
#include "storage/table/csr_chunked_node_group.h"
#include "storage/table/csr_node_group.h"
#include "storage/table/rel_table_data.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace processor {

void CountRelTable::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* /*context*/) {
    countVector = resultSet->getValueVector(countOutputPos).get();
    hasExecuted = false;
    totalCount = 0;
}

// Count rels by using CSR metadata, accounting for deletions and uncommitted data.
// This is more efficient than scanning through all edges.
bool CountRelTable::getNextTuplesInternal(ExecutionContext* context) {
    if (hasExecuted) {
        return false;
    }

    auto transaction = Transaction::Get(*context->clientContext);
    auto* memoryManager = context->clientContext->getDatabase()->getMemoryManager();

    for (auto* relTable : relTables) {
        // Get the RelTableData for the specified direction
        auto* relTableData = relTable->getDirectedTableData(direction);
        auto numNodeGroups = relTableData->getNumNodeGroups();
        auto* csrLengthColumn = relTableData->getCSRLengthColumn();

        // For each node group in the rel table
        for (node_group_idx_t nodeGroupIdx = 0; nodeGroupIdx < numNodeGroups; nodeGroupIdx++) {
            auto* nodeGroup = relTableData->getNodeGroup(nodeGroupIdx);
            if (!nodeGroup) {
                continue;
            }

            auto& csrNodeGroup = nodeGroup->cast<CSRNodeGroup>();

            // Count from persistent (checkpointed) data
            if (auto* persistentGroup = csrNodeGroup.getPersistentChunkedGroup()) {
                // Sum the actual relationship lengths from the CSR header instead of using
                // getNumRows() which includes dummy rows added for CSR offset array gaps
                auto& csrPersistentGroup = persistentGroup->cast<ChunkedCSRNodeGroup>();
                auto& csrHeader = csrPersistentGroup.getCSRHeader();

                // Get the number of nodes in this CSR header
                auto numNodes = csrHeader.length->getNumValues();
                if (numNodes == 0) {
                    continue;
                }

                // Create an in-memory chunk to scan the CSR length column into
                auto lengthChunk =
                    ColumnChunkFactory::createColumnChunkData(*memoryManager, LogicalType::UINT64(),
                        false /*enableCompression*/, StorageConfig::NODE_GROUP_SIZE,
                        ResidencyState::IN_MEMORY, false /*initializeToZero*/);

                // Initialize scan state and scan the length column from disk
                ChunkState chunkState;
                csrHeader.length->initializeScanState(chunkState, csrLengthColumn);
                csrLengthColumn->scan(chunkState, lengthChunk.get(), 0 /*offsetInChunk*/, numNodes);

                // Sum all the lengths
                auto* lengthData = reinterpret_cast<const uint64_t*>(lengthChunk->getData());
                row_idx_t groupRelCount = 0;
                for (offset_t i = 0; i < numNodes; ++i) {
                    groupRelCount += lengthData[i];
                }
                totalCount += groupRelCount;

                // Subtract deletions from persistent data
                if (persistentGroup->hasVersionInfo()) {
                    auto numDeletions =
                        persistentGroup->getNumDeletions(transaction, 0, groupRelCount);
                    totalCount -= numDeletions;
                }
            }

            // Count in-memory committed data (not yet checkpointed)
            // This data is stored in chunkedGroups within the NodeGroup
            auto numChunkedGroups = csrNodeGroup.getNumChunkedGroups();
            for (node_group_idx_t i = 0; i < numChunkedGroups; i++) {
                auto* chunkedGroup = csrNodeGroup.getChunkedNodeGroup(i);
                if (chunkedGroup) {
                    auto numRows = chunkedGroup->getNumRows();
                    totalCount += numRows;
                    // Subtract deletions from in-memory committed data
                    if (chunkedGroup->hasVersionInfo()) {
                        auto numDeletions = chunkedGroup->getNumDeletions(transaction, 0, numRows);
                        totalCount -= numDeletions;
                    }
                }
            }
        }

        // Add uncommitted insertions from local storage
        if (transaction->isWriteTransaction()) {
            if (auto* localTable =
                    transaction->getLocalStorage()->getLocalTable(relTable->getTableID())) {
                auto& localRelTable = localTable->cast<LocalRelTable>();
                // Count entries in the CSR index for this direction.
                // We can't use getNumTotalRows() because it includes deleted rows.
                auto& csrIndex = localRelTable.getCSRIndex(direction);
                for (const auto& [nodeOffset, rowIndices] : csrIndex) {
                    totalCount += rowIndices.size();
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

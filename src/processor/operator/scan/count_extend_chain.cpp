#include "processor/operator/scan/count_extend_chain.h"

#include "common/system_config.h"
#include "common/types/int128_t.h"
#include "main/client_context.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace processor {

offset_t CountExtendChain::getOffsetUpperBound(NodeTable* nodeTable) const {
    const auto numGroups = nodeTable->getNumNodeGroups();
    if (numGroups == 0) {
        return 0;
    }
    return (numGroups - 1) * StorageConfig::NODE_GROUP_SIZE +
           nodeTable->getNumTuplesInNodeGroup(numGroups - 1);
}

void CountExtendChain::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* /*context*/) {
    countVector = resultSet->getValueVector(countOutputPos).get();
    hasExecuted = false;
    totalCount = 0;
    counts.clear();
}

void CountExtendChain::processBatch(uint64_t hopIdx, table_id_t fromTableID, table_id_t toTableID,
    bool isLastHop, const RelTableScanState& scanState, const ValueVector& nodeIDVector,
    const ValueVector& nbrVector) {
    const auto outputSize = scanState.outState->getSelVector().getSelSize();
    if (outputSize == 0) {
        return;
    }
    auto& cCur = counts[hopIdx][fromTableID];
    const auto& boundSel = nodeIDVector.state->getSelVector();
    const auto boundSelSize = boundSel.getSelSize();
    const auto& outSel = scanState.outState->getSelVector();
    if (isLastHop) {
        // The final count is the number of paths of the full chain length, i.e. the sum of
        // c_{N-1}[src] over all visible edges of the last hop.
        if (boundSelSize > 1) {
            // Multi-parent packed batch: served parents are in the (unflat) bound selection
            // vector; rows [packedChildOffsets[p], packedChildOffsets[p+1]) belong to parent p.
            const auto& packedChildOffsets = scanState.packedChildOffsets;
            DASSERT(packedChildOffsets.size() == size_t(boundSelSize) + 1);
            DASSERT(packedChildOffsets.back() == outputSize);
            for (auto p = 0u; p < boundSelSize; ++p) {
                const auto u = nodeIDVector.readNodeOffset(boundSel[p]);
                totalCount += cCur[u] * (packedChildOffsets[p + 1] - packedChildOffsets[p]);
            }
        } else {
            DASSERT(boundSelSize == 1);
            const auto u = nodeIDVector.readNodeOffset(boundSel[0]);
            totalCount += cCur[u] * outputSize;
        }
        return;
    }
    auto& cNext = counts[hopIdx + 1][toTableID];
    if (boundSelSize > 1) {
        const auto& packedChildOffsets = scanState.packedChildOffsets;
        DASSERT(packedChildOffsets.size() == size_t(boundSelSize) + 1);
        DASSERT(packedChildOffsets.back() == outputSize);
        for (auto p = 0u; p < boundSelSize; ++p) {
            const auto u = nodeIDVector.readNodeOffset(boundSel[p]);
            const auto cu = cCur[u];
            if (cu == 0) {
                continue;
            }
            for (auto r = packedChildOffsets[p]; r < packedChildOffsets[p + 1]; ++r) {
                cNext[nbrVector.readNodeOffset(outSel[r])] += cu;
            }
        }
    } else {
        // Single-parent batch (also the contract of the in-memory/local scan paths): all rows
        // of the batch belong to the bound node pointed at by the flat bound selection vector.
        DASSERT(boundSelSize == 1);
        const auto u = nodeIDVector.readNodeOffset(boundSel[0]);
        const auto cu = cCur[u];
        if (cu == 0) {
            return;
        }
        for (auto r = 0u; r < outputSize; ++r) {
            cNext[nbrVector.readNodeOffset(outSel[r])] += cu;
        }
    }
}

bool CountExtendChain::getNextTuplesInternal(ExecutionContext* context) {
    if (hasExecuted) {
        return false;
    }
    auto transaction = Transaction::Get(*context->clientContext);
    auto* memoryManager = MemoryManager::Get(*context->clientContext);

    const auto numHops = hops.size();
    // counts[j]: per-node partial path counts for chain position j (0..numHops). Position 0 is
    // ones over the chain's source node table; position numHops is never materialized (the last
    // hop accumulates directly into the total).
    counts.resize(numHops);
    totalCount = 0;

    // Allocate zero count vectors for chain positions 1..numHops-1, sized to the full node
    // group grid so that offsets referencing any allocated node remain in bounds even when the
    // tail node group is partially deleted (deleted tail offsets never carry visible edges).
    for (auto j = 0u; j + 1 < numHops; ++j) {
        auto& nextMap = counts[j + 1];
        for (auto i = 0u; i < hops[j].relTables.size(); ++i) {
            const auto toTableID = hops[j].toNodeTableIDs[i];
            if (!nextMap.contains(toTableID)) {
                const auto* nodeTable = nodeTables.at(toTableID);
                nextMap.emplace(toTableID,
                    std::vector<int64_t>(
                        nodeTable->getNumNodeGroups() * StorageConfig::NODE_GROUP_SIZE, 0));
            }
        }
    }
    // Position 0: ones over the first hop's from-side node tables.
    for (auto i = 0u; i < hops[0].relTables.size(); ++i) {
        const auto fromTableID = hops[0].fromNodeTableIDs[i];
        if (!counts[0].contains(fromTableID)) {
            const auto* nodeTable = nodeTables.at(fromTableID);
            counts[0].emplace(fromTableID,
                std::vector<int64_t>(nodeTable->getNumNodeGroups() * StorageConfig::NODE_GROUP_SIZE,
                    1));
        }
    }

    for (auto j = 0u; j < numHops; ++j) {
        const auto isLastHop = j + 1 == numHops;
        auto& hop = hops[j];
        for (auto i = 0u; i < hop.relTables.size(); ++i) {
            auto* relTable = hop.relTables[i];
            const auto fromTableID = hop.fromNodeTableIDs[i];
            const auto toTableID = hop.toNodeTableIDs[i];
            auto* fromNodeTable = nodeTables.at(fromTableID);

            auto nodeIDVector = std::make_shared<ValueVector>(LogicalType::INTERNAL_ID(),
                memoryManager, std::make_shared<DataChunkState>());
            auto nbrVector = std::make_shared<ValueVector>(LogicalType::INTERNAL_ID(),
                memoryManager, std::make_shared<DataChunkState>());
            std::vector<ValueVector*> outVectors{nbrVector.get()};
            RelTableScanState scanState(*memoryManager, nodeIDVector.get(), outVectors,
                nbrVector->state);
            scanState.setToTable(transaction, relTable, {NBR_ID_COLUMN_ID}, {},
                hop.scanDirections[i]);
            scanState.packedMultiParentScan = true;

            // Iterate the from-side node offsets sequentially. Batches of one vector capacity
            // aligned to the table origin never span more than one node group.
            const auto offsetUpper = getOffsetUpperBound(fromNodeTable);
            for (auto start = offset_t{0}; start < offsetUpper; start += DEFAULT_VECTOR_CAPACITY) {
                const auto n = static_cast<sel_t>(
                    std::min<offset_t>(DEFAULT_VECTOR_CAPACITY, offsetUpper - start));
                nodeIDVector->state->setToUnflat();
                nodeIDVector->state->getSelVectorUnsafe().setToUnfiltered(n);
                for (auto k = 0u; k < n; ++k) {
                    nodeIDVector->setValue<nodeID_t>(k, nodeID_t{start + k, fromTableID});
                }
                relTable->initScanState(transaction, scanState);
                while (relTable->scan(transaction, scanState)) {
                    processBatch(j, fromTableID, toTableID, isLastHop, scanState, *nodeIDVector,
                        *nbrVector);
                }
            }
        }
    }

    hasExecuted = true;

    // Write the count to the output vector (single value).
    countVector->state->getSelVectorUnsafe().setToUnfiltered(1);
    countVector->setNull(0, false);
    switch (countVector->dataType.getPhysicalType()) {
    case PhysicalTypeID::INT64:
        countVector->setValue<int64_t>(0, totalCount);
        break;
    case PhysicalTypeID::INT128:
        countVector->setValue<int128_t>(0, int128_t{totalCount});
        break;
    default:
        UNREACHABLE_CODE;
    }
    return true;
}

} // namespace processor
} // namespace lbug

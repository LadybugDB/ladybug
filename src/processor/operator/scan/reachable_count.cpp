#include "processor/operator/scan/reachable_count.h"

#include <unordered_set>

#include "main/client_context.h"
#include "processor/execution_context.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::graph;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace processor {

void ReachableCount::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) {
    countVector = resultSet->getValueVector(countOutputPos).get();
    hasExecuted = false;

    // Build the on-disk graph from the stored graph entry. This reuses the same neighbor-scan
    // machinery that the recursive extend / GDS framework relies on.
    graph = std::make_unique<OnDiskGraph>(context->clientContext, graphEntry.copy());

    // Pre-compute the forward rel surfaces leaving from the bound node table and prepare one
    // scan state per physical rel table so they can be reused across all frontier nodes.
    relInfos = graph->getRelInfos(boundTableID);
    for (auto& relInfo : relInfos) {
        auto scanState = graph->prepareRelScan(*relInfo.relGroupEntry, relInfo.relTableID,
            relInfo.dstTableID, /*relProperties=*/std::vector<std::string>{});
        scanStates.push_back(std::move(scanState));
    }
}

offset_t ReachableCount::computeReachableCount() {
    // Distinct neighbor node offsets reachable by a walk of some length in [lowerBound,
    // upperBound].
    std::unordered_set<offset_t> seen;
    std::vector<offset_t> frontier = startOffsets;
    if (lowerBound == 0) {
        for (auto offset : startOffsets) {
            seen.insert(offset);
        }
    }
    for (uint16_t d = 1; d <= upperBound; ++d) {
        std::unordered_set<offset_t> nextFrontier;
        for (auto offset : frontier) {
            nodeID_t nodeID{offset, boundTableID};
            for (auto& scanState : scanStates) {
                for (auto chunk : graph->scanFwd(nodeID, *scanState)) {
                    chunk.forEach([&](auto neighbors, auto /*propertyVectors*/, auto i) {
                        auto nbr = neighbors[i];
                        if (nbr.tableID != nbrTableID) {
                            return;
                        }
                        nextFrontier.insert(nbr.offset);
                        if (d >= lowerBound) {
                            seen.insert(nbr.offset);
                        }
                    });
                }
            }
        }
        if (nextFrontier.empty()) {
            break;
        }
        frontier.assign(nextFrontier.begin(), nextFrontier.end());
    }
    return static_cast<offset_t>(seen.size());
}

bool ReachableCount::getNextTuplesInternal(ExecutionContext*) {
    if (hasExecuted) {
        return false;
    }
    auto count = computeReachableCount();
    countVector->state->getSelVectorUnsafe().setToUnfiltered(1);
    countVector->setValue<int64_t>(0, static_cast<int64_t>(count));
    hasExecuted = true;
    return true;
}

} // namespace processor
} // namespace lbug

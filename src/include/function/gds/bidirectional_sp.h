#pragma once

#include <functional>

#include "function/gds/bfs_graph.h"
#include "function/gds/gds_frontier.h"

namespace lbug {
namespace processor {
struct ExecutionContext;
}
namespace function {

// Where a single-pair search writes its result for the existing output writers.
struct BidirectionalSPConfig {
    // Receives the destination's path length.
    SPFrontierPair* frontierPair = nullptr;
    common::NodeOffsetMaskMap* pathNodeMask = nullptr;
    // Receives the parents of the shortest paths. Null when only lengths are returned.
    BaseBFSGraph* pathGraph = nullptr;
    // Keep every shortest path instead of one.
    bool allShortest = false;
    // Receives the number of shortest paths. Set only when the output counts them.
    std::function<void(common::nodeID_t, uint64_t)> setPathCount;
};

// Searches from both ends when outputMask holds exactly one node other than the source. Returns
// true when the search finished, with nothing written if there is no path or the destination
// fails the node predicate. Returns false, with nothing written, when the mask holds another
// number of nodes, when it holds only the source, or when the search reaches more edges than its
// budget allows; the caller then runs the one-sided search.
bool runBidirectionalSinglePair(processor::ExecutionContext* context, graph::Graph* graph,
    common::ExtendDirection direction, uint64_t maxLength, common::nodeID_t source,
    common::NodeOffsetMaskMap* outputMask, const BidirectionalSPConfig& config);

} // namespace function
} // namespace lbug

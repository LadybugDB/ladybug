#include "function/gds/bidirectional_sp.h"

#include <algorithm>
#include <optional>

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/constants.h"
#include "common/exception/interrupt.h"
#include "common/types/internal_id_util.h"
#include "graph/graph.h"
#include "graph/graph_entry.h"
#include "main/client_context.h"
#include "processor/execution_context.h"

using namespace lbug::common;
using namespace lbug::graph;
using namespace lbug::processor;

namespace lbug {
namespace function {

namespace {

// The search runs on one thread. Once more than this many edges have passed the relationship
// predicate it gives up and the parallel one-sided search runs instead, so a pair whose ends meet
// only after most of a large graph first pays for scanning about this many edges.
constexpr uint64_t MAX_SCANNED_EDGES = 1 << 16;

// Parents are indexed with 32 bits. There is at most one per edge reached, and the budget above
// stops the search far below that limit.
constexpr uint32_t NO_PARENT = UINT32_MAX;

// A parent is one step closer to its side's end. isFwd is true when the edge is stored from the
// parent to the child.
struct Parent {
    nodeID_t node;
    relID_t edge;
    bool isFwd;
    uint32_t next;
};

struct Visit {
    uint16_t dist;
    // Shortest paths from this side's end. Counted only when the output needs it.
    uint64_t numPaths;
    uint32_t firstParent;
};

struct SearchSide {
    bool fromTarget;
    node_id_map_t<Visit> visits;
    std::vector<Parent> parents;
    // The nodes at distance depth, which the next expansion scans from.
    std::vector<nodeID_t> level;
    uint16_t depth = 0;

    SearchSide(nodeID_t end, bool fromTarget) : fromTarget{fromTarget} {
        visits.emplace(end, Visit{0, 1, NO_PARENT});
        level.push_back(end);
    }

    const Visit* find(nodeID_t node) const {
        const auto it = visits.find(node);
        return it == visits.end() ? nullptr : &it->second;
    }

    void addParent(Visit& child, nodeID_t parent, relID_t edge, bool isFwd) {
        parents.push_back(Parent{parent, edge, isFwd, child.firstParent});
        child.firstParent = static_cast<uint32_t>(parents.size() - 1);
    }
};

// An edge from a node reached from the source to a node reached from the target. isFwd is true
// when the edge is stored in that direction.
struct Meeting {
    nodeID_t sourceSide;
    nodeID_t targetSide;
    relID_t edge;
    bool isFwd;
};

struct RelScan {
    table_id_t boundTableID;
    bool isFwd;
    std::unique_ptr<NbrScanState> state;
};

// A disabled mask admits every node of its table, so the destination is single only when every
// mask is enabled and one node is masked in all of them.
std::optional<nodeID_t> getSingleMaskedNode(NodeOffsetMaskMap* mask) {
    if (mask == nullptr || mask->getNumMaskedNode() != 1) {
        return std::nullopt;
    }
    std::optional<nodeID_t> result;
    for (auto& [tableID, tableMask] : mask->getMasks()) {
        if (tableMask == nullptr || !tableMask->isEnabled()) {
            return std::nullopt;
        }
        if (tableMask->getNumMaskedNodes() != 0) {
            result = nodeID_t{tableMask->collectMaskedNodes(1)[0], tableID};
        }
    }
    return result;
}

// The two sides walk edges in opposite directions, so every rel table must store both.
bool storesBothDirections(Graph* graph) {
    for (auto& nodeInfo : graph->getGraphEntry()->nodeInfos) {
        for (auto& rel : graph->getRelInfos(nodeInfo.entry->getTableID())) {
            const auto& relGroup = rel.relGroupEntry->constCast<catalog::RelGroupCatalogEntry>();
            if (relGroup.getRelDataDirections().size() < 2) {
                return false;
            }
        }
    }
    return true;
}

class BidirectionalSearch {
public:
    BidirectionalSearch(ExecutionContext* context, Graph* graph, ExtendDirection direction,
        uint16_t maxLength, nodeID_t source, nodeID_t target, const BidirectionalSPConfig& config)
        : context{context}, graph{graph}, maxLength{maxLength}, config{config},
          recordPaths{config.pathGraph != nullptr}, countPaths{config.setPathCount != nullptr},
          target{target}, fromSource{source, false}, fromTarget{target, true} {
        prepareScans(direction);
    }

    // Returns false when the budget ran out before the search finished.
    // An empty level means that end has no more reachable nodes, so no path is left to find.
    bool run() {
        while (!found() && fromSource.depth + fromTarget.depth < maxLength &&
               !fromSource.level.empty() && !fromTarget.level.empty()) {
            const auto expandTarget = fromTarget.level.size() <= fromSource.level.size();
            auto& side = expandTarget ? fromTarget : fromSource;
            auto& other = expandTarget ? fromSource : fromTarget;
            if (!expand(side, other)) {
                return false;
            }
        }
        return true;
    }

    void writeResult() {
        if (!found()) {
            return;
        }
        auto* frontier = config.frontierPair->getFrontier();
        frontier->pinTableID(target.tableID);
        frontier->addNode(target, best);
        if (countPaths) {
            config.setPathCount(target, numShortestPaths);
        }
        if (recordPaths) {
            writePaths();
        }
    }

    bool found() const { return best != FRONTIER_UNVISITED; }

private:
    void prepareScans(ExtendDirection direction) {
        const auto properties = recordPaths ? std::vector<std::string>{InternalKeyword::ID} :
                                              std::vector<std::string>{};
        for (auto isFwd : {true, false}) {
            for (auto& nodeInfo : graph->getGraphEntry()->nodeInfos) {
                for (auto& rel : graph->getRelInfos(nodeInfo.entry->getTableID())) {
                    const auto boundTableID = isFwd ? rel.srcTableID : rel.dstTableID;
                    const auto nbrTableID = isFwd ? rel.dstTableID : rel.srcTableID;
                    scans.push_back(RelScan{boundTableID, isFwd,
                        graph->prepareRelScan(*rel.relGroupEntry, rel.relTableID, nbrTableID,
                            properties)});
                }
            }
        }
        // The source side follows the query's direction and the target side goes against it.
        for (auto& scan : scans) {
            switch (direction) {
            case ExtendDirection::FWD: {
                (scan.isFwd ? sourceScans : targetScans).push_back(&scan);
            } break;
            case ExtendDirection::BWD: {
                (scan.isFwd ? targetScans : sourceScans).push_back(&scan);
            } break;
            case ExtendDirection::BOTH: {
                sourceScans.push_back(&scan);
                targetScans.push_back(&scan);
            } break;
            }
        }
    }

    bool expand(SearchSide& side, const SearchSide& other) {
        const auto newDist = static_cast<uint16_t>(side.depth + 1);
        std::vector<nodeID_t> nextLevel;
        for (auto* scan : side.fromTarget ? targetScans : sourceScans) {
            for (const auto bound : side.level) {
                if (bound.tableID != scan->boundTableID) {
                    continue;
                }
                const auto boundPaths = countPaths ? side.find(bound)->numPaths : 0;
                auto edges = scan->isFwd ? graph->scanFwd(bound, *scan->state) :
                                           graph->scanBwd(bound, *scan->state);
                for (const auto chunk : edges) {
                    if (context->clientContext->interrupted()) {
                        throw InterruptException{};
                    }
                    numScannedEdges += chunk.size();
                    if (numScannedEdges > MAX_SCANNED_EDGES) {
                        return false;
                    }
                    chunk.forEach([&](auto neighbors, auto propertyVectors, auto i) {
                        const auto edge = recordPaths ?
                                              propertyVectors[0]->template getValue<relID_t>(i) :
                                              relID_t{};
                        visitEdge(side, other, bound, boundPaths, neighbors[i], edge, scan->isFwd,
                            newDist, nextLevel);
                    });
                }
            }
        }
        side.level = std::move(nextLevel);
        side.depth = newDist;
        return true;
    }

    void visitEdge(SearchSide& side, const SearchSide& other, nodeID_t bound, uint64_t boundPaths,
        nodeID_t nbr, relID_t edge, bool scannedFwd, uint16_t newDist,
        std::vector<nodeID_t>& nextLevel) {
        // Checked before the node predicate: the source is exempt from it, and every other node
        // the other side holds already passed it.
        if (const auto* otherVisit = other.find(nbr)) {
            addMeeting(side, bound, nbr, edge, scannedFwd, newDist + otherVisit->dist,
                boundPaths * otherVisit->numPaths);
            return;
        }
        if (config.pathNodeMask != nullptr && !config.pathNodeMask->valid(nbr)) {
            return;
        }
        auto [it, inserted] = side.visits.try_emplace(nbr, Visit{newDist, 0, NO_PARENT});
        auto& visit = it->second;
        if (visit.dist != newDist) {
            return;
        }
        if (inserted) {
            nextLevel.push_back(nbr);
        }
        visit.numPaths += boundPaths;
        if (recordPaths && (inserted || config.allShortest)) {
            side.addParent(visit, bound, edge, scannedFwd);
        }
    }

    void addMeeting(const SearchSide& side, nodeID_t bound, nodeID_t nbr, relID_t edge,
        bool scannedFwd, uint16_t length, uint64_t numPaths) {
        if (length > best) {
            return;
        }
        if (length < best) {
            best = length;
            numShortestPaths = 0;
            meetings.clear();
        }
        numShortestPaths += numPaths;
        if (!recordPaths) {
            return;
        }
        if (side.fromTarget) {
            meetings.push_back(Meeting{nbr, bound, edge, !scannedFwd});
        } else {
            meetings.push_back(Meeting{bound, nbr, edge, scannedFwd});
        }
    }

    void addPathParent(uint16_t iter, nodeID_t parent, relID_t edge, nodeID_t child, bool isFwd) {
        auto* pathGraph = config.pathGraph;
        pathGraph->pinTableID(child.tableID);
        if (block == nullptr || !block->hasSpace()) {
            block = pathGraph->addNewBlock();
        }
        if (config.allShortest) {
            pathGraph->addParent(iter, parent, edge, child, isFwd, block);
        } else {
            pathGraph->addSingleParent(iter, parent, edge, child, isFwd, block);
        }
    }

    // The output writers walk parents from the destination back to the source, with each
    // parent stored at its child's distance from the source. Only the shortest paths are
    // copied: the source side's parents as they are, the target side's reversed.
    void writePaths() {
        const auto numMeetings = config.allShortest ? meetings.size() : 1;
        std::vector<nodeID_t> sourceStack, targetStack;
        for (auto i = 0u; i < numMeetings; ++i) {
            const auto& meeting = meetings[i];
            const auto targetDist = fromTarget.find(meeting.targetSide)->dist;
            addPathParent(best - targetDist, meeting.sourceSide, meeting.edge, meeting.targetSide,
                meeting.isFwd);
            sourceStack.push_back(meeting.sourceSide);
            targetStack.push_back(meeting.targetSide);
        }
        node_id_set_t done;
        while (!sourceStack.empty()) {
            const auto child = sourceStack.back();
            sourceStack.pop_back();
            if (!done.insert(child).second) {
                continue;
            }
            const auto* visit = fromSource.find(child);
            for (auto idx = visit->firstParent; idx != NO_PARENT;) {
                const auto& parent = fromSource.parents[idx];
                addPathParent(visit->dist, parent.node, parent.edge, child, parent.isFwd);
                sourceStack.push_back(parent.node);
                idx = parent.next;
            }
        }
        done.clear();
        while (!targetStack.empty()) {
            const auto node = targetStack.back();
            targetStack.pop_back();
            if (!done.insert(node).second) {
                continue;
            }
            const auto* visit = fromTarget.find(node);
            for (auto idx = visit->firstParent; idx != NO_PARENT;) {
                const auto& parent = fromTarget.parents[idx];
                addPathParent(best - visit->dist + 1, node, parent.edge, parent.node,
                    !parent.isFwd);
                targetStack.push_back(parent.node);
                idx = parent.next;
            }
        }
    }

    ExecutionContext* context;
    Graph* graph;
    uint16_t maxLength;
    const BidirectionalSPConfig& config;
    bool recordPaths;
    bool countPaths;
    nodeID_t target;
    SearchSide fromSource;
    SearchSide fromTarget;
    std::vector<RelScan> scans;
    std::vector<RelScan*> sourceScans;
    std::vector<RelScan*> targetScans;
    uint64_t numScannedEdges = 0;
    uint16_t best = FRONTIER_UNVISITED;
    uint64_t numShortestPaths = 0;
    std::vector<Meeting> meetings;
    ObjectBlock<ParentList>* block = nullptr;
};

} // namespace

bool runBidirectionalSinglePair(ExecutionContext* context, Graph* graph, ExtendDirection direction,
    uint64_t maxLength, nodeID_t source, NodeOffsetMaskMap* outputMask,
    const BidirectionalSPConfig& config) {
    const auto target = getSingleMaskedNode(outputMask);
    if (!target.has_value() || source == target.value() || !storesBothDirections(graph)) {
        return false;
    }
    // Like the one-sided search, never reach a destination that fails the node predicate.
    if (config.pathNodeMask != nullptr && !config.pathNodeMask->valid(target.value())) {
        return true;
    }
    const auto length =
        static_cast<uint16_t>(std::min<uint64_t>(maxLength, FRONTIER_UNVISITED - 1));
    BidirectionalSearch search{context, graph, direction, length, source, target.value(), config};
    if (!search.run()) {
        return false;
    }
    search.writeResult();
    return true;
}

} // namespace function
} // namespace lbug

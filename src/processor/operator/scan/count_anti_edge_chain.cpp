#include "processor/operator/scan/count_anti_edge_chain.h"

#include "common/system_config.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace processor {

namespace {

// CSR adjacency of one rel scan direction over the mid node table: offsets has midVecSize + 1
// entries, nbrs holds the neighbor node offsets.
struct DirAdjacency {
    std::vector<offset_t> offsets;
    std::vector<offset_t> nbrs;
};

// The CSR directions scanned for an extend in the given direction. A BOTH extend enumerates
// the FWD rows followed by the BWD rows of each bound node (no dedup), matching LogicalExtend.
struct DirComponents {
    RelDataDirection dirs[2];
    uint8_t numDirs;
};

DirComponents componentsOf(ExtendDirection dir) {
    switch (dir) {
    case ExtendDirection::FWD:
        return {{RelDataDirection::FWD}, 1};
    case ExtendDirection::BWD:
        return {{RelDataDirection::BWD}, 1};
    case ExtendDirection::BOTH:
        return {{RelDataDirection::FWD, RelDataDirection::BWD}, 2};
    default:
        UNREACHABLE_CODE;
    }
}

ExtendDirection reverse(ExtendDirection dir) {
    switch (dir) {
    case ExtendDirection::FWD:
        return ExtendDirection::BWD;
    case ExtendDirection::BWD:
        return ExtendDirection::FWD;
    case ExtendDirection::BOTH:
        return ExtendDirection::BOTH;
    default:
        UNREACHABLE_CODE;
    }
}

} // namespace

void CountAntiEdgeChain::initLocalStateInternal(ResultSet* resultSet,
    ExecutionContext* /*context*/) {
    countVector = resultSet->getValueVector(countOutputPos).get();
    hasExecuted = false;
}

template<typename Func>
void CountAntiEdgeChain::scanRelRows(RelTable* relTable, RelDataDirection direction,
    NodeTable* boundNodeTable, Transaction* transaction, MemoryManager* memoryManager,
    Func&& callback) {
    auto nodeIDVector = std::make_shared<ValueVector>(LogicalType::INTERNAL_ID(), memoryManager,
        std::make_shared<DataChunkState>());
    auto nbrVector = std::make_shared<ValueVector>(LogicalType::INTERNAL_ID(), memoryManager,
        std::make_shared<DataChunkState>());
    std::vector<ValueVector*> outVectors{nbrVector.get()};
    RelTableScanState scanState(*memoryManager, nodeIDVector.get(), outVectors, nbrVector->state);
    scanState.setToTable(transaction, relTable, {NBR_ID_COLUMN_ID}, {}, direction);
    scanState.packedMultiParentScan = true;

    const auto offsetUpper = getOffsetUpperBound(boundNodeTable);
    for (auto start = offset_t{0}; start < offsetUpper; start += DEFAULT_VECTOR_CAPACITY) {
        const auto n =
            static_cast<sel_t>(std::min<offset_t>(DEFAULT_VECTOR_CAPACITY, offsetUpper - start));
        nodeIDVector->state->setToUnflat();
        nodeIDVector->state->getSelVectorUnsafe().setToUnfiltered(n);
        for (auto k = 0u; k < n; ++k) {
            nodeIDVector->setValue<nodeID_t>(k, nodeID_t{start + k, boundNodeTable->getTableID()});
        }
        relTable->initScanState(transaction, scanState);
        while (relTable->scan(transaction, scanState)) {
            callback(scanState, *nodeIDVector, *nbrVector);
        }
    }
}

// Invoke callback(boundOffset, nbrOffset) for every visible row of the current scan batch,
// resolving the bound node of each output row through the packed multi-parent contract (or the
// single-parent flat contract used by the in-memory/local scan paths).
template<typename CB>
static void forEachScanRow(const RelTableScanState& scanState, const ValueVector& nodeIDVector,
    const ValueVector& nbrVector, CB&& callback) {
    const auto outputSize = scanState.outState->getSelVector().getSelSize();
    if (outputSize == 0) {
        return;
    }
    const auto& boundSel = nodeIDVector.state->getSelVector();
    const auto boundSelSize = boundSel.getSelSize();
    const auto& outSel = scanState.outState->getSelVector();
    if (boundSelSize > 1) {
        const auto& packedChildOffsets = scanState.packedChildOffsets;
        DASSERT(packedChildOffsets.size() == size_t(boundSelSize) + 1);
        DASSERT(packedChildOffsets.back() == outputSize);
        for (auto p = 0u; p < boundSelSize; ++p) {
            const auto u = nodeIDVector.readNodeOffset(boundSel[p]);
            for (auto r = packedChildOffsets[p]; r < packedChildOffsets[p + 1]; ++r) {
                callback(u, nbrVector.readNodeOffset(outSel[r]));
            }
        }
    } else {
        DASSERT(boundSelSize == 1);
        const auto u = nodeIDVector.readNodeOffset(boundSel[0]);
        for (auto r = 0u; r < outputSize; ++r) {
            callback(u, nbrVector.readNodeOffset(outSel[r]));
        }
    }
}

std::vector<int64_t> CountAntiEdgeChain::computeSuffixCounts(Transaction* transaction,
    MemoryManager* memoryManager) {
    // B[k]: per-node suffix path counts at suffix position k (0 = the n2/mid side, numHops =
    // the far end). B[numHops] = ones over the far-end node table; B[k][v] = sum over rows
    // (v -> w) of hop[k] (enumerated with the scan direction keyed by the n-side) of B[k+1][w].
    // Only B[0] (= D over the mid node table) feeds the arithmetic; intermediate vectors are
    // dropped as we walk backward.
    const auto vectorSizeFor = [](NodeTable* t) {
        return t->getNumNodeGroups() * StorageConfig::NODE_GROUP_SIZE;
    };
    const auto numHops = suffixHops.size();
    std::vector<std::vector<int64_t>> vecs(numHops + 1);
    NodeTable* farTable = suffixHops[numHops - 1].toNodeTables[0];
    vecs[numHops] = std::vector<int64_t>(vectorSizeFor(farTable), 1);
    for (auto k = numHops; k > 0; --k) {
        auto& hop = suffixHops[k - 1];
        NodeTable* fromTable = hop.fromNodeTables[0];
        vecs[k - 1] = std::vector<int64_t>(vectorSizeFor(fromTable), 0);
        auto& Bk = vecs[k - 1];
        auto& Bk1 = vecs[k];
        for (auto i = 0u; i < hop.relTables.size(); ++i) {
            scanRelRows(hop.relTables[i], hop.scanDirections[i], hop.fromNodeTables[i], transaction,
                memoryManager,
                [&](const RelTableScanState& scanState, const ValueVector& nodeIDVector,
                    const ValueVector& nbrVector) {
                    forEachScanRow(scanState, nodeIDVector, nbrVector,
                        [&](offset_t v, offset_t w) { Bk[v] += Bk1[w]; });
                });
        }
        vecs[k] = std::vector<int64_t>();
    }
    return vecs[0];
}

bool CountAntiEdgeChain::getNextTuplesInternal(ExecutionContext* context) {
    if (hasExecuted) {
        return false;
    }
    auto transaction = Transaction::Get(*context->clientContext);
    auto* memoryManager = MemoryManager::Get(*context->clientContext);

    const auto midVecSize = midNodeTable->getNumNodeGroups() * StorageConfig::NODE_GROUP_SIZE;
    const auto offsetUpper = getOffsetUpperBound(midNodeTable);

    // Step 1: D[v] = number of suffix paths starting at v (over the mid node table).
    const auto D = computeSuffixCounts(transaction, memoryManager);

    // Step 2: directional adjacency of the anti-edge rel over the mid node table. outAdj[v]
    // holds the FWD rows with src=v; inAdj[v] holds the BWD rows with dst=v. All enumeration
    // below is row-level: a BOTH hop enumerates the FWD rows followed by the BWD rows of each
    // bound node (no dedup), matching the extend operator's semantics.
    DirAdjacency outAdj;
    DirAdjacency inAdj;
    outAdj.offsets.assign(midVecSize + 1, 0);
    inAdj.offsets.assign(midVecSize + 1, 0);
    auto scanPairs = [&](RelDataDirection dir, auto&& callback) {
        scanRelRows(antiRelTable, dir, midNodeTable, transaction, memoryManager,
            [&](const RelTableScanState& scanState, const ValueVector& nodeIDVector,
                const ValueVector& nbrVector) {
                forEachScanRow(scanState, nodeIDVector, nbrVector, callback);
            });
    };
    scanPairs(RelDataDirection::FWD, [&](offset_t u, offset_t /*v*/) { ++outAdj.offsets[u + 1]; });
    scanPairs(RelDataDirection::BWD, [&](offset_t u, offset_t /*v*/) { ++inAdj.offsets[u + 1]; });
    for (auto v = 0u; v < midVecSize; ++v) {
        outAdj.offsets[v + 1] += outAdj.offsets[v];
        inAdj.offsets[v + 1] += inAdj.offsets[v];
    }
    outAdj.nbrs.resize(outAdj.offsets[midVecSize]);
    inAdj.nbrs.resize(inAdj.offsets[midVecSize]);
    {
        std::vector<offset_t> outCursor(outAdj.offsets.begin(), outAdj.offsets.end() - 1);
        std::vector<offset_t> inCursor(inAdj.offsets.begin(), inAdj.offsets.end() - 1);
        scanPairs(RelDataDirection::FWD,
            [&](offset_t u, offset_t v) { outAdj.nbrs[outCursor[u]++] = v; });
        scanPairs(RelDataDirection::BWD,
            [&](offset_t u, offset_t v) { inAdj.nbrs[inCursor[u]++] = v; });
    }
    const auto& listFor = [&](RelDataDirection dir) -> const DirAdjacency& {
        return dir == RelDataDirection::FWD ? outAdj : inAdj;
    };
    const auto degreeOf = [&](ExtendDirection dir, offset_t v) -> offset_t {
        offset_t deg = 0;
        const auto components = componentsOf(dir);
        for (auto ci = 0u; ci < components.numDirs; ++ci) {
            const auto& lst = listFor(components.dirs[ci]);
            deg += lst.offsets[v + 1] - lst.offsets[v];
        }
        return deg;
    };

    // Step 3: T = all chain tuples (n0, n1, n2) weighted by D(n2): n2 iterates the n2-hop rows
    // in direction chainN2Dir; each such row pairs with every n0-hop row of n1.
    int64_t T = 0;
    {
        const auto n2Components = componentsOf(chainN2Dir);
        for (auto ci = 0u; ci < n2Components.numDirs; ++ci) {
            const auto& lst = listFor(n2Components.dirs[ci]);
            for (auto v = 0u; v < offsetUpper; ++v) {
                const auto degN0 = degreeOf(chainN0Dir, v);
                if (degN0 == 0) {
                    continue;
                }
                for (auto i = lst.offsets[v]; i < lst.offsets[v + 1]; ++i) {
                    T += static_cast<int64_t>(degN0) * D[lst.nbrs[i]];
                }
            }
        }
    }

    // Step 4: S = tuples with n0 = n2 = p, which are excluded by the id(n0) <> id(n2)
    // predicate when it is present. For each mid node n1 and node p, the number of
    // (n0-row, n2-row) combinations with both endpoints p is multN2(p) * multN0(p).
    int64_t S = 0;
    if (hasNotEquals) {
        std::vector<uint32_t> stampPass(midVecSize, 0);
        std::vector<offset_t> stampCnt(midVecSize, 0);
        uint32_t pass = 0;
        const auto n2Components = componentsOf(chainN2Dir);
        const auto n0Components = componentsOf(chainN0Dir);
        for (auto v = 0u; v < offsetUpper; ++v) {
            ++pass;
            for (auto ci = 0u; ci < n2Components.numDirs; ++ci) {
                const auto& lst = listFor(n2Components.dirs[ci]);
                for (auto i = lst.offsets[v]; i < lst.offsets[v + 1]; ++i) {
                    const auto p = lst.nbrs[i];
                    if (stampPass[p] != pass) {
                        stampPass[p] = pass;
                        stampCnt[p] = 0;
                    }
                    ++stampCnt[p];
                }
            }
            for (auto ci = 0u; ci < n0Components.numDirs; ++ci) {
                const auto& lst = listFor(n0Components.dirs[ci]);
                for (auto i = lst.offsets[v]; i < lst.offsets[v + 1]; ++i) {
                    const auto p = lst.nbrs[i];
                    if (stampPass[p] == pass) {
                        S += static_cast<int64_t>(stampCnt[p]) * D[p];
                    }
                }
            }
        }
    }

    // Step 5: A = tuples where the anti-edge row exists. For each anti-edge row (n0, n2)
    // enumerated in direction antiEdgeDir, the number of chain tuples through it is
    // N'(n0,n2) = sum over mid nodes x of multRevN0(n0,x) * multRevN2(n2,x), weighted by
    // D(n2). Computed by stamping n2's rev(chainN2Dir) list once per n2 and scanning each row
    // partner n0's rev(chainN0Dir) list against the stamps.
    int64_t A = 0;
    {
        std::vector<uint32_t> stampPass(midVecSize, 0);
        std::vector<offset_t> stampCnt(midVecSize, 0);
        std::vector<uint32_t> partnerPass(midVecSize, 0);
        uint32_t pass = 0;
        uint32_t partnerPassEpoch = 0;
        const auto revN0 = reverse(chainN0Dir);
        const auto revN2 = reverse(chainN2Dir);
        // Anti-edge rows (n0, n2) in direction antiEdgeDir, grouped by n2: FWD rows are
        // (n0 -> n2) so n0 is an in-neighbor of n2; BWD rows are (n2 -> n0) so n0 is an
        // out-neighbor of n2; BOTH rows are both orientations.
        const auto partnerComponents =
            componentsOf(antiEdgeDir == ExtendDirection::FWD ? ExtendDirection::BWD :
                         antiEdgeDir == ExtendDirection::BWD ? ExtendDirection::FWD :
                                                               ExtendDirection::BOTH);
        const auto revN2Components = componentsOf(revN2);
        const auto revN0Components = componentsOf(revN0);
        for (auto n2 = 0u; n2 < offsetUpper; ++n2) {
            const auto dN2 = D[n2];
            if (dN2 == 0) {
                continue;
            }
            ++pass;
            // The anti-edge match is a boolean mark per (n0, n2) key pair, so each distinct
            // row partner n0 of n2 is subtracted once even if it appears in both the in and
            // out lists (BOTH anti-edge) or via duplicate rows.
            ++partnerPassEpoch;
            for (auto ci = 0u; ci < revN2Components.numDirs; ++ci) {
                const auto& lst = listFor(revN2Components.dirs[ci]);
                for (auto i = lst.offsets[n2]; i < lst.offsets[n2 + 1]; ++i) {
                    const auto x = lst.nbrs[i];
                    if (stampPass[x] != pass) {
                        stampPass[x] = pass;
                        stampCnt[x] = 0;
                    }
                    ++stampCnt[x];
                }
            }
            for (auto ci = 0u; ci < partnerComponents.numDirs; ++ci) {
                const auto& partners = listFor(partnerComponents.dirs[ci]);
                for (auto i = partners.offsets[n2]; i < partners.offsets[n2 + 1]; ++i) {
                    const auto n0 = partners.nbrs[i];
                    if (partnerPass[n0] == partnerPassEpoch) {
                        continue;
                    }
                    partnerPass[n0] = partnerPassEpoch;
                    int64_t numPaths = 0;
                    for (auto cj = 0u; cj < revN0Components.numDirs; ++cj) {
                        const auto& lst = listFor(revN0Components.dirs[cj]);
                        for (auto j = lst.offsets[n0]; j < lst.offsets[n0 + 1]; ++j) {
                            const auto x = lst.nbrs[j];
                            if (stampPass[x] == pass) {
                                numPaths += static_cast<int64_t>(stampCnt[x]);
                            }
                        }
                    }
                    A += numPaths * dN2;
                }
            }
        }
    }

    const auto count = T - S - A;

    hasExecuted = true;
    countVector->state->getSelVectorUnsafe().setToUnfiltered(1);
    countVector->setNull(0, false);
    countVector->setValue<int64_t>(0, count);
    return true;
}

} // namespace processor
} // namespace lbug

#include "graph/graph.h"

#include "common/system_config.h"

namespace lbug::graph {
NbrScanState::Chunk::Chunk(std::span<const common::nodeID_t> nbrNodes,
    common::SelectionVector& selVector,
    std::span<const std::shared_ptr<common::ValueVector>> propertyVectors)
    : nbrNodes{nbrNodes}, selVector{selVector}, propertyVectors{propertyVectors} {
    DASSERT(nbrNodes.size() == common::DEFAULT_VECTOR_CAPACITY);
}

VertexScanState::Chunk::Chunk(std::span<const common::nodeID_t> nodeIDs,
    std::span<const std::shared_ptr<common::ValueVector>> propertyVectors)
    : nodeIDs{nodeIDs}, propertyVectors{propertyVectors} {
    DASSERT(nodeIDs.size() <= common::DEFAULT_VECTOR_CAPACITY);
}
} // namespace lbug::graph

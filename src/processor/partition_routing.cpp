#include "processor/partition_routing.h"

#include "common/types/types.h"
#include "function/hash/vector_hash_functions.h"

namespace lbug {
namespace processor {

void computePartitionIndexes(const common::ValueVector& keyVector, uint64_t numPartitions,
    std::vector<uint64_t>& outIndexes) {
    DASSERT(numPartitions > 0);
    const auto& selVector = keyVector.state->getSelVector();
    const auto numTuples = selVector.getSelSize();
    outIndexes.resize(numTuples);

    auto hashVector = std::make_unique<common::ValueVector>(common::LogicalType::UINT64());
    hashVector->state = keyVector.state;
    function::VectorHashFunction::computeHash(keyVector, selVector, *hashVector, selVector);
    for (auto i = 0u; i < numTuples; ++i) {
        const auto pos = selVector[i];
        outIndexes[i] = hashVector->getValue<common::hash_t>(pos) % numPartitions;
    }
}

} // namespace processor
} // namespace lbug
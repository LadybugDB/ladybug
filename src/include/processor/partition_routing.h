#pragma once

#include <cstdint>
#include <vector>

#include "common/vector/value_vector.h"

namespace lbug {
namespace processor {

// Computes a partition index in [0, numPartitions) for every selected row of `keyVector`.
//
// HASH partitions use the same value hashing as the built-in HASH() function, so a given
// partition-key value always lands in the same partition. RANGE partitions currently reuse the
// same deterministic hash until declarative range bounds are implemented (see
// docs/partitioning.md); reads on the parent union over all partitions, so data remains
// findable regardless of the routing function.
void computePartitionIndexes(const common::ValueVector& keyVector, uint64_t numPartitions,
    std::vector<uint64_t>& outIndexes);

} // namespace processor
} // namespace lbug
#pragma once

#include "common/api.h"
#include "common/types/types.h"

namespace lbug {
namespace processor {

// Reference merge used for mixed-table lists, balanced lists, and differential tests.
LBUG_API common::sel_t intersectNodeIDsScalar(common::nodeID_t* left, common::sel_t leftCount,
    const common::nodeID_t* right, common::sel_t rightCount, common::sel_t* leftPositions,
    common::sel_t* rightPositions);

// Selects the skew-aware same-table fast path when it is profitable and otherwise uses the
// reference merge. Both inputs must be sorted by nodeID_t's lexicographic ordering, and leftCount
// must be less than or equal to rightCount.
LBUG_API common::sel_t intersectNodeIDs(common::nodeID_t* left, common::sel_t leftCount,
    const common::nodeID_t* right, common::sel_t rightCount, common::sel_t* leftPositions,
    common::sel_t* rightPositions);

} // namespace processor
} // namespace lbug

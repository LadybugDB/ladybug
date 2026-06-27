#pragma once

#include "processor/operator/physical_operator.h"

namespace lbug {
namespace processor {

// Utilities for inspecting a PhysicalOperator tree.
//
// getOrderPreservation replaces the previous logical-plan walker
// (LogicalPlanUtil::hasOrderByOnDataPath, since removed) that hand-maintained
// a list of operator types which "reset" the ordering question. The walker
// here asks each physical operator for its declared OrderPreservationType
// metadata (operatorOrder() / sourceOrder()), so adding a new reordering
// operator that forgets to declare its semantics safely under-classifies it
// as NO_ORDER (the default) — the cheap batch-index collector path.
class PhysicalPlanUtil {
public:
    // Walks the physical plan and returns the strongest order preservation
    // the root operator can guarantee for the rows arriving at a downstream
    // sink (e.g. result collector).
    //
    // Walks child[0] only. Ladybug physical plans have a single data-flow
    // edge per operator; any "side inputs" (e.g. the build side of a hash
    // join) are attached as siblings via separate operator subtrees (sink
    // + side-channel pull), not as additional children of the data-flow
    // operator.
    //
    // Returns FIXED_ORDER if any operator on the data path returns
    // FIXED_ORDER. Otherwise returns the root's operatorOrder() (default
    // NO_ORDER).
    static OrderPreservationType getOrderPreservation(const PhysicalOperator& op);
};

} // namespace processor
} // namespace lbug
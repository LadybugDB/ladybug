#include "processor/physical_plan_util.h"

namespace lbug {
namespace processor {

OrderPreservationType PhysicalPlanUtil::getOrderPreservation(const PhysicalOperator& op) {
    if (op.isSource()) {
        return op.sourceOrder();
    }
    if (op.getNumChildren() == 0) {
        return op.operatorOrder();
    }
    // FIXED_ORDER short-circuits: if any operator on the path is fixed-order,
    // the result is fixed-order regardless of what comes after.
    const auto childType = getOrderPreservation(*op.getChild(0));
    if (childType == OrderPreservationType::FIXED_ORDER) {
        return childType;
    }
    return op.operatorOrder();
}

} // namespace processor
} // namespace lbug
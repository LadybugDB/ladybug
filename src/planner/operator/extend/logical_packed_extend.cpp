#include "planner/operator/extend/logical_packed_extend.h"

namespace lbug {
namespace planner {

void LogicalPackedExtend::computeFactorizedSchema() {
    copyChildSchema(0);
    const auto boundGroupPos = schema->getGroupPos(*boundNode->getInternalID());
    const auto nbrGroupPos = schema->createChildGroup(boundGroupPos, boundNode->getInternalID(),
        nbrNode->getInternalID());
    schema->insertToGroupAndScope(nbrNode->getInternalID(), nbrGroupPos);
    for (auto& property : getProperties()) {
        schema->insertToGroupAndScope(property, nbrGroupPos);
    }
    if (rel->hasDirectionExpr()) {
        schema->insertToGroupAndScope(rel->getDirectionExpr(), nbrGroupPos);
    }
}

void LogicalPackedExtend::computeFlatSchema() {
    LogicalExtend::computeFlatSchema();
}

std::unique_ptr<LogicalOperator> LogicalPackedExtend::copy() {
    auto extend = std::make_unique<LogicalPackedExtend>(boundNode, nbrNode, rel, direction,
        extendFromSource_, getProperties(), children[0]->copy(), cardinality);
    extend->setPropertyPredicates(copyVector(getPropertyPredicates()));
    extend->setScanNbrID(shouldScanNbrID());
    return extend;
}

} // namespace planner
} // namespace lbug

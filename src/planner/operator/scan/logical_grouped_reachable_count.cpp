#include "planner/operator/scan/logical_grouped_reachable_count.h"

namespace lbug {
namespace planner {

void LogicalGroupedReachableCount::computeFactorizedSchema() {
    createEmptySchema();
    // Multi-row source: one group holding keys + aggregates, not single-state (unlike the
    // single-row count operators).
    auto groupPos = schema->createGroup();
    for (auto& key : keys) {
        schema->insertToGroupAndScope(key, groupPos);
    }
    if (countExpr != nullptr) {
        schema->insertToGroupAndScope(countExpr, groupPos);
    }
    if (avgExpr != nullptr) {
        schema->insertToGroupAndScope(avgExpr, groupPos);
    }
}

void LogicalGroupedReachableCount::computeFlatSchema() {
    createEmptySchema();
    auto groupPos = schema->createGroup();
    for (auto& key : keys) {
        schema->insertToGroupAndScope(key, groupPos);
    }
    if (countExpr != nullptr) {
        schema->insertToGroupAndScope(countExpr, groupPos);
    }
    if (avgExpr != nullptr) {
        schema->insertToGroupAndScope(avgExpr, groupPos);
    }
}

std::string LogicalGroupedReachableCount::getExpressionsForPrinting() const {
    std::string result;
    for (auto& key : keys) {
        if (!result.empty()) {
            result += ",";
        }
        result += key->toString();
    }
    return result;
}

} // namespace planner
} // namespace lbug

#include "binder/expression/node_rel_expression.h"
#include "binder/expression/property_expression.h"
#include "planner/operator/scan/logical_scan_node_table.h"
#include "planner/planner.h"

using namespace lbug::common;
using namespace lbug::binder;

namespace lbug {
namespace planner {

static expression_vector removeInternalIDProperty(const expression_vector& expressions) {
    expression_vector result;
    for (auto expr : expressions) {
        if (expr->constCast<PropertyExpression>().isInternalID()) {
            continue;
        }
        result.push_back(expr);
    }
    return result;
}

void Planner::appendScanNodeTable(std::shared_ptr<Expression> nodeID,
    std::vector<table_id_t> tableIDs, const expression_vector& properties, LogicalPlan& plan,
    const NodeOrRelExpression* nodeOrRel) {
    auto propertiesToScan_ = removeInternalIDProperty(properties);
    auto scan = make_shared<LogicalScanNodeTable>(std::move(nodeID), std::move(tableIDs),
        propertiesToScan_);
    // Build tableDBMap from node/rel expression if available
    if (nodeOrRel) {
        std::unordered_map<common::table_id_t, std::string> dbMap;
        for (auto* entry : nodeOrRel->getEntries()) {
            auto dbName = nodeOrRel->getDbName(entry);
            if (!dbName.empty()) {
                dbMap[entry->getTableID()] = dbName;
            }
        }
        scan->setTableDBMap(std::move(dbMap));
    }
    scan->computeFactorizedSchema();
    scan->setCardinality(cardinalityEstimator.estimateScanNode(*scan));
    plan.setLastOperator(std::move(scan));
}

} // namespace planner
} // namespace lbug

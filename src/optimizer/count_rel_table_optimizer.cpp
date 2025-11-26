#include "optimizer/count_rel_table_optimizer.h"

#include "binder/expression/aggregate_function_expression.h"
#include "function/aggregate/count_star.h"
#include "main/client_context.h"
#include "planner/operator/extend/logical_extend.h"
#include "planner/operator/logical_aggregate.h"
#include "planner/operator/logical_projection.h"
#include "planner/operator/scan/logical_count_rel_table.h"
#include "planner/operator/scan/logical_scan_node_table.h"

using namespace lbug::common;
using namespace lbug::planner;
using namespace lbug::binder;
using namespace lbug::catalog;

namespace lbug {
namespace optimizer {

void CountRelTableOptimizer::rewrite(LogicalPlan* plan) {
    visitOperator(plan->getLastOperator());
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::visitOperator(
    const std::shared_ptr<LogicalOperator>& op) {
    // bottom-up traversal
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        op->setChild(i, visitOperator(op->getChild(i)));
    }
    auto result = visitOperatorReplaceSwitch(op);
    result->computeFlatSchema();
    return result;
}

bool CountRelTableOptimizer::isSimpleCountStar(LogicalOperator* op) const {
    if (op->getOperatorType() != LogicalOperatorType::AGGREGATE) {
        return false;
    }
    auto& aggregate = op->constCast<LogicalAggregate>();

    // Must have no keys (i.e., a simple aggregate without GROUP BY)
    if (aggregate.hasKeys()) {
        return false;
    }

    // Must have exactly one aggregate expression
    auto aggregates = aggregate.getAggregates();
    if (aggregates.size() != 1) {
        return false;
    }

    // Must be COUNT_STAR
    auto& aggExpr = aggregates[0];
    if (aggExpr->expressionType != ExpressionType::AGGREGATE_FUNCTION) {
        return false;
    }
    auto& aggFuncExpr = aggExpr->constCast<AggregateFunctionExpression>();
    if (aggFuncExpr.getFunction().name != function::CountStarFunction::name) {
        return false;
    }

    // COUNT_STAR should not be DISTINCT (conceptually it doesn't make sense)
    if (aggFuncExpr.isDistinct()) {
        return false;
    }

    return true;
}

bool CountRelTableOptimizer::canOptimize(LogicalOperator* aggregate) const {
    // Pattern we're looking for:
    // AGGREGATE (COUNT_STAR, no keys)
    //   -> PROJECTION (empty expressions or pass-through)
    //      -> EXTEND (single rel table, no properties scanned)
    //         -> SCAN_NODE_TABLE (no properties scanned)
    //
    // Note: The projection between aggregate and extend might be empty or
    // just projecting the count expression.

    auto* current = aggregate->getChild(0).get();

    // Skip any projections (they should be empty or just for count)
    while (current->getOperatorType() == LogicalOperatorType::PROJECTION) {
        auto& proj = current->constCast<LogicalProjection>();
        // Empty projection is okay, it's just a passthrough
        if (!proj.getExpressionsToProject().empty()) {
            // If projection has expressions, they should all be aggregate expressions
            // (which means they're just passing through the count)
            for (auto& expr : proj.getExpressionsToProject()) {
                if (expr->expressionType != ExpressionType::AGGREGATE_FUNCTION) {
                    return false;
                }
            }
        }
        current = current->getChild(0).get();
    }

    // Now we should have EXTEND
    if (current->getOperatorType() != LogicalOperatorType::EXTEND) {
        return false;
    }
    auto& extend = current->constCast<LogicalExtend>();

    // The rel should be a single table (not multi-labeled)
    auto rel = extend.getRel();
    if (rel->isMultiLabeled()) {
        return false;
    }

    // Check if we're scanning any properties (we can only optimize when no properties needed)
    if (!extend.getProperties().empty()) {
        return false;
    }

    // The child of extend should be SCAN_NODE_TABLE
    auto* extendChild = current->getChild(0).get();
    if (extendChild->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE) {
        return false;
    }
    auto& scanNode = extendChild->constCast<LogicalScanNodeTable>();

    // Check if node scan has any properties (we can only optimize when no properties needed)
    if (!scanNode.getProperties().empty()) {
        return false;
    }

    return true;
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::visitAggregateReplace(
    std::shared_ptr<LogicalOperator> op) {
    if (!isSimpleCountStar(op.get())) {
        return op;
    }

    if (!canOptimize(op.get())) {
        return op;
    }

    // Find the EXTEND operator
    auto* current = op->getChild(0).get();
    while (current->getOperatorType() == LogicalOperatorType::PROJECTION) {
        current = current->getChild(0).get();
    }

    KU_ASSERT(current->getOperatorType() == LogicalOperatorType::EXTEND);
    auto& extend = current->constCast<LogicalExtend>();
    auto rel = extend.getRel();

    // Get the rel group entry
    KU_ASSERT(rel->getNumEntries() == 1);
    auto* relGroupEntry = rel->getEntry(0)->ptrCast<RelGroupCatalogEntry>();

    // Get all rel table IDs
    std::vector<table_id_t> relTableIDs;
    for (auto& info : relGroupEntry->getRelEntryInfos()) {
        relTableIDs.push_back(info.oid);
    }

    // Get the count expression from the original aggregate
    auto& aggregate = op->constCast<LogicalAggregate>();
    auto countExpr = aggregate.getAggregates()[0];

    // Create the new COUNT_REL_TABLE operator
    auto countRelTable =
        std::make_shared<LogicalCountRelTable>(relGroupEntry, std::move(relTableIDs), countExpr);
    countRelTable->computeFlatSchema();

    return countRelTable;
}

} // namespace optimizer
} // namespace lbug

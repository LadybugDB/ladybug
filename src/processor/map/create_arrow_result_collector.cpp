#include "binder/expression/property_expression.h"
#include "binder/expression/scalar_function_expression.h"
#include "function/schema/vector_node_rel_functions.h"
#include "processor/operator/arrow_result_collector.h"
#include "processor/physical_plan_util.h"
#include "processor/plan_mapper.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "transaction/transaction.h"

using namespace lbug::common;

namespace lbug {
namespace processor {

static bool isProjectedRowIDExpr(const binder::Expression& expr) {
    if (expr.expressionType != ExpressionType::FUNCTION) {
        return false;
    }
    const auto& scalarFunc = expr.constCast<binder::ScalarFunctionExpression>();
    if (scalarFunc.getFunction().name != function::OffsetFunction::name ||
        scalarFunc.getNumChildren() != 1) {
        return false;
    }
    const auto child = scalarFunc.getChild(0);
    if (child->expressionType != ExpressionType::PROPERTY) {
        return false;
    }
    const auto& property = child->constCast<binder::PropertyExpression>();
    return property.isInternalID();
}

static CSRTrackingInfo getCSRTrackingInfo(const binder::expression_vector& expressions) {
    CSRTrackingInfo info;
    std::vector<idx_t> rowIDExprPositions;
    for (auto i = 0u; i < expressions.size(); ++i) {
        if (isProjectedRowIDExpr(*expressions[i])) {
            rowIDExprPositions.push_back(i);
        }
    }
    if (rowIDExprPositions.size() == 2) {
        info.srcRowIDColIdx = rowIDExprPositions[0];
        info.dstRowIDColIdx = rowIDExprPositions[1];
    } else if (rowIDExprPositions.size() == 3) {
        info.srcRowIDColIdx = rowIDExprPositions[0];
        info.relRowIDColIdx = rowIDExprPositions[1];
        info.dstRowIDColIdx = rowIDExprPositions[2];
    }
    return info;
}

std::unique_ptr<PhysicalOperator> PlanMapper::createArrowResultCollector(
    ArrowResultConfig arrowConfig, const binder::expression_vector& expressions,
    planner::Schema* schema, std::unique_ptr<PhysicalOperator> prevOperator,
    OrderPreservationType orderPreservation) {
    std::vector<DataPos> columnDataPos;
    std::vector<LogicalType> columnTypes;
    for (auto& expr : expressions) {
        columnDataPos.push_back(getDataPos(*expr, *schema));
        columnTypes.push_back(expr->getDataType().copy());
    }
    auto sharedState = std::make_shared<ArrowResultCollectorSharedState>();
    sharedState->requireDeterministicOrder =
        (orderPreservation == OrderPreservationType::FIXED_ORDER);
    auto csrTrackingInfo = getCSRTrackingInfo(expressions);
    if (csrTrackingInfo.enabled()) {
        // Look up the source node table's total row count so we can pad
        // trailing empty rows in the CSR indptr (nodes with zero outgoing
        // edges must still have an indptr slot).
        const auto& srcExpr = *expressions[csrTrackingInfo.srcRowIDColIdx];
        const auto& scalarFunc = srcExpr.constCast<binder::ScalarFunctionExpression>();
        const auto& propExpr = scalarFunc.getChild(0)->constCast<binder::PropertyExpression>();
        auto tableID = propExpr.getSingleTableID();
        auto trx = transaction::Transaction::Get(*clientContext);
        auto table = storage::StorageManager::Get(*clientContext)->getTable(tableID);
        auto nodeTable = table->ptrCast<storage::NodeTable>();
        csrTrackingInfo.numSourceRows = static_cast<int64_t>(nodeTable->getNumTotalRows(trx));
    }
    auto opInfo = ArrowResultCollectorInfo(arrowConfig.chunkSize, columnDataPos,
        std::move(columnTypes), csrTrackingInfo, orderPreservation);
    auto printInfo = OPPrintInfo::EmptyInfo();
    if (csrTrackingInfo.enabled() &&
        (expressions.size() == 2 || (expressions.size() == 3 && csrTrackingInfo.hasRelRowID()))) {
        auto op = std::make_unique<DirectArrowResultCollector>(sharedState, std::move(opInfo),
            std::move(prevOperator), getOperatorID(), std::move(printInfo));
        op->setDescriptor(std::make_unique<ResultSetDescriptor>(schema));
        return op;
    }
    auto op = std::make_unique<ArrowResultCollector>(sharedState, std::move(opInfo),
        std::move(prevOperator), getOperatorID(), std::move(printInfo));
    op->setDescriptor(std::make_unique<ResultSetDescriptor>(schema));
    return op;
}

} // namespace processor
} // namespace lbug

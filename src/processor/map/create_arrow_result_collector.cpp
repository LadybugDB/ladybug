#include "binder/expression/property_expression.h"
#include "binder/expression/scalar_function_expression.h"
#include "function/schema/vector_node_rel_functions.h"
#include "processor/operator/arrow_result_collector.h"
#include "processor/plan_mapper.h"

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
    planner::Schema* schema, std::unique_ptr<PhysicalOperator> prevOperator) {
    std::vector<DataPos> columnDataPos;
    std::vector<LogicalType> columnTypes;
    for (auto& expr : expressions) {
        columnDataPos.push_back(getDataPos(*expr, *schema));
        columnTypes.push_back(expr->getDataType().copy());
    }
    auto sharedState = std::make_shared<ArrowResultCollectorSharedState>();
    auto opInfo = ArrowResultCollectorInfo(arrowConfig.chunkSize, columnDataPos,
        std::move(columnTypes), getCSRTrackingInfo(expressions));
    auto printInfo = OPPrintInfo::EmptyInfo();
    auto op = std::make_unique<ArrowResultCollector>(sharedState, std::move(opInfo),
        std::move(prevOperator), getOperatorID(), std::move(printInfo));
    op->setDescriptor(std::make_unique<ResultSetDescriptor>(schema));
    return op;
}

} // namespace processor
} // namespace lbug

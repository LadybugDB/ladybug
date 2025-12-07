#include "planner/operator/logical_table_function_call.h"

namespace lbug {
namespace planner {

void LogicalTableFunctionCall::computeFlatSchema() {
    createEmptySchema();
    auto groupPos = schema->createGroup();
    for (auto& expr : bindData->columns) {
        schema->insertToGroupAndScope(expr, groupPos);
    }
}

void LogicalTableFunctionCall::computeFactorizedSchema() {
    createEmptySchema();
    auto groupPos = schema->createGroup();
    for (auto& expr : bindData->columns) {
        schema->insertToGroupAndScope(expr, groupPos);
    }
}

std::unique_ptr<OPPrintInfo> LogicalTableFunctionCall::getPrintInfo() const {
    return std::make_unique<LogicalTableFunctionCallPrintInfo>(getExpressionsForPrinting());
}

} // namespace planner
} // namespace lbug

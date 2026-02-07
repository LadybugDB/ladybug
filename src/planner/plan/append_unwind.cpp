#include "binder/expression/literal_expression.h"
#include "binder/expression_visitor.h"
#include "binder/query/reading_clause/bound_unwind_clause.h"
#include "expression_evaluator/expression_evaluator_utils.h"
#include "planner/operator/logical_unwind.h"
#include "planner/planner.h"

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace planner {

void Planner::appendUnwind(const BoundReadingClause& readingClause, LogicalPlan& plan) {
    auto& unwindClause = ku_dynamic_cast<const BoundUnwindClause&>(readingClause);
    auto inExpr = unwindClause.getInExpr();
    if (ConstantExpressionVisitor::isConstant(*inExpr)) {
        auto value =
            evaluator::ExpressionEvaluatorUtils::evaluateConstantExpression(inExpr, clientContext);
        inExpr = std::make_shared<LiteralExpression>(std::move(value), inExpr->getUniqueName());
    }
    auto unwind = make_shared<LogicalUnwind>(inExpr, unwindClause.getOutExpr(),
        unwindClause.getIDExpr(), plan.getLastOperator());
    appendFlattens(unwind->getGroupsPosToFlatten(), plan);
    unwind->setChild(0, plan.getLastOperator());
    unwind->computeFactorizedSchema();
    plan.setLastOperator(unwind);
}

} // namespace planner
} // namespace lbug

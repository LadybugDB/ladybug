#include "function/aggregate/count.h"

#include "binder/expression/case_expression.h"
#include "binder/expression/expression_util.h"
#include "binder/expression/literal_expression.h"
#include "binder/expression/node_expression.h"
#include "binder/expression/rel_expression.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::binder;

namespace lbug {
namespace function {

void CountFunction::updateAll(uint8_t* state_, ValueVector* input, uint64_t multiplicity,
    InMemOverflowBuffer* /*overflowBuffer*/) {
    // state_ may point into a packed factorized-table tuple without alignment padding.
    addToCount(state_, multiplicity * input->countNonNull());
}

static std::shared_ptr<Expression> rewriteNodeToID(const std::shared_ptr<Expression>& arg) {
    if (ExpressionUtil::isNodePattern(*arg)) {
        return arg->constCast<NodeExpression>().getInternalID();
    }
    if (ExpressionUtil::isRelPattern(*arg)) {
        return arg->constCast<RelExpression>().getInternalID();
    }
    return nullptr;
}

// Rebuilds CASE WHEN ... THEN <node/rel> ... ELSE NULL END over internal IDs, preserving
// the unique name. Sound only when every value branch holds a node/relationship (unique
// per row, null exactly when unbound) and the ELSE branch is NULL (a non-null sentinel
// of another type could collide with an ID after the mapping). Returns nullptr when
// inapplicable.
static std::shared_ptr<Expression> rewriteCaseOnIDs(const std::shared_ptr<Expression>& arg) {
    if (arg->expressionType != ExpressionType::CASE_ELSE) {
        return nullptr;
    }
    auto& caseExpr = arg->constCast<CaseExpression>();
    auto elseExpr = caseExpr.getElseExpression();
    if (elseExpr->expressionType != ExpressionType::LITERAL ||
        !elseExpr->constCast<LiteralExpression>().isNull()) {
        return nullptr;
    }
    auto rebuilt = std::make_shared<CaseExpression>(LogicalType::INTERNAL_ID(), elseExpr,
        arg->getUniqueName());
    for (auto i = 0u; i < caseExpr.getNumCaseAlternatives(); ++i) {
        auto alternative = caseExpr.getCaseAlternative(i);
        auto rewritten = rewriteNodeToID(alternative->thenExpression);
        if (rewritten == nullptr) {
            return nullptr;
        }
        rebuilt->addCaseAlternative(alternative->whenExpression, std::move(rewritten));
    }
    return rebuilt;
}

void CountFunction::paramRewriteFunc(expression_vector& arguments) {
    DASSERT(arguments.size() == 1);
    if (auto rewritten = rewriteNodeToID(arguments[0])) {
        arguments[0] = std::move(rewritten);
    } else if (auto rebuilt = rewriteCaseOnIDs(arguments[0])) {
        arguments[0] = std::move(rebuilt);
    }
}

function_set CountFunction::getFunctionSet() {
    function_set result;
    for (auto& type : LogicalTypeUtils::getAllValidLogicTypeIDs()) {
        for (auto isDistinct : std::vector<bool>{true, false}) {
            auto func = AggregateFunctionUtils::getAggFunc<CountFunction>(name, type,
                LogicalTypeID::INT64, isDistinct, paramRewriteFunc);
            func->needToHandleNulls = true;
            result.push_back(std::move(func));
        }
    }
    return result;
}

} // namespace function
} // namespace lbug

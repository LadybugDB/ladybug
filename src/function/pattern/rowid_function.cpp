#include "binder/expression/expression_util.h"
#include "binder/expression/node_expression.h"
#include "binder/expression/rel_expression.h"
#include "binder/expression_binder.h"
#include "function/rewrite_function.h"
#include "function/schema/vector_node_rel_functions.h"
#include "function/struct/vector_struct_functions.h"

using namespace lbug::common;
using namespace lbug::binder;

namespace lbug {
namespace function {

static std::shared_ptr<binder::Expression> rewriteFunc(const RewriteFunctionBindInput& input) {
    KU_ASSERT(input.arguments.size() == 1);
    std::shared_ptr<Expression> idExpr;
    auto param = input.arguments[0].get();
    if (ExpressionUtil::isNodePattern(*param)) {
        auto node = param->constPtrCast<NodeExpression>();
        idExpr = node->getInternalID()->copy();
    } else if (ExpressionUtil::isRelPattern(*param)) {
        auto rel = param->constPtrCast<RelExpression>();
        idExpr = rel->getPropertyExpression(InternalKeyword::ID)->copy();
    } else {
        auto extractKey = input.expressionBinder->createLiteralExpression(InternalKeyword::ID);
        idExpr = input.expressionBinder->bindScalarFunctionExpression(
            {input.arguments[0], extractKey}, StructExtractFunctions::name);
    }
    return input.expressionBinder->bindScalarFunctionExpression({idExpr}, OffsetFunction::name);
}

function_set RowIDFunction::getFunctionSet() {
    function_set functionSet;
    functionSet.push_back(std::make_unique<RewriteFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::NODE}, rewriteFunc));
    functionSet.push_back(std::make_unique<RewriteFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::REL}, rewriteFunc));
    return functionSet;
}

} // namespace function
} // namespace lbug

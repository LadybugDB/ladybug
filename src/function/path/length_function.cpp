#include "binder/expression/case_expression.h"
#include "binder/expression/expression_util.h"
#include "binder/expression/path_expression.h"
#include "binder/expression/rel_expression.h"
#include "binder/expression/scalar_function_expression.h"
#include "binder/expression_binder.h"
#include "common/exception/binder.h"
#include "common/types/value/value.h"
#include "function/list/vector_list_functions.h"
#include "function/path/vector_path_functions.h"
#include "function/rewrite_function.h"

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace function {

// A fixed-length path has a constant length, but a path from an OPTIONAL MATCH can be null, and
// then so is its length. The path itself is tested because WITH carries it without its rels.
static std::shared_ptr<Expression> nullIfUnmatched(const RewriteFunctionBindInput& input,
    std::shared_ptr<Expression> length) {
    auto path = input.arguments[0];
    if (!path->constCast<PathExpression>().isOptional()) {
        return length;
    }
    auto binder = input.expressionBinder;
    auto isNull = binder->bindNullOperatorExpression(ExpressionType::IS_NULL, {path});
    auto nullLength = binder->implicitCastIfNecessary(binder->createNullLiteralExpression(),
        length->getDataType().copy());
    auto result = std::make_shared<CaseExpression>(length->getDataType().copy(), length,
        ScalarFunctionExpression::getUniqueName(LengthFunction::name, input.arguments));
    result->addCaseAlternative(isNull, nullLength);
    return result;
}

static std::shared_ptr<Expression> rewriteFunc(const RewriteFunctionBindInput& input) {
    DASSERT(input.arguments.size() == 1);
    auto param = input.arguments[0].get();
    auto binder = input.expressionBinder;
    if (param->expressionType == ExpressionType::PATH) {
        int64_t numRels = 0u;
        bool hasRecursive = false;
        for (auto& child : param->getChildren()) {
            if (ExpressionUtil::isRelPattern(*child)) {
                numRels++;
            } else if (ExpressionUtil::isRecursiveRelPattern(*child)) {
                hasRecursive = true;
                break;
            }
        }
        if (!hasRecursive) {
            // Fixed-length paths have a constant length with no runtime dependency, so keep
            // the cheap literal rewrite. A path from an OPTIONAL MATCH can still be null,
            // and then so is its length (the path itself is tested because WITH carries
            // it without its rels).
            auto numRelsExpression = binder->createLiteralExpression(Value(numRels));
            return nullIfUnmatched(input, std::move(numRelsExpression));
        }
        // Variable-length paths fall through to the path-based rewrite below. The previous
        // rewrite summed the hidden `_LENGTH` properties of the recursive rels, but those
        // belong to the rel variables, which `WITH p` does not project, so the mapper later
        // failed with `Cannot evaluate expression with type PROPERTY.`
    } else if (param->getDataType().getLogicalTypeID() != LogicalTypeID::RECURSIVE_REL) {
        throw BinderException("LENGTH() expects a path or recursive relationship as input. "
                              "Use LENGTH(path) where path is a MATCH path variable, e.g. MATCH "
                              "p = "
                              "(a)-[]->(b) RETURN LENGTH(p).");
    }
    // LENGTH(p) == SIZE(RELS(p)): the rels list lives inside the path/recursive-rel value
    // itself, so this depends only on the path variable and survives `WITH p`, `WITH *`,
    // aliasing (`WITH p AS q`), and `UNWIND collect(p) AS p`.
    auto relsExpr = binder->bindScalarFunctionExpression({input.arguments[0]}, RelsFunction::name);
    return binder->bindScalarFunctionExpression({relsExpr}, SizeFunction::name);
}

function_set LengthFunction::getFunctionSet() {
    function_set result;
    auto function = std::make_unique<RewriteFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::RECURSIVE_REL}, rewriteFunc);
    result.push_back(std::move(function));
    return result;
}

} // namespace function
} // namespace lbug

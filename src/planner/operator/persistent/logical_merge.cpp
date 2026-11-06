#include "planner/operator/persistent/logical_merge.h"

#include <unordered_set>

#include "binder/expression/node_expression.h"
#include "binder/expression/rel_expression.h"
#include "binder/expression/scalar_function_expression.h"
#include "common/cast.h"
#include "function/cast/cast_function_bind_data.h"
#include "planner/operator/factorization/flatten_resolver.h"

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace planner {

// A CAST between integer types keeps distinct values distinct or fails. Casts are recognised by
// their bind data, since a user function can be named CAST_*. A casting function registered for a
// user-defined integer type binds the same way and is taken to be injective too.
static bool isIntegerCast(const Expression& expression) {
    if (expression.expressionType != ExpressionType::FUNCTION) {
        return false;
    }
    auto bindData = expression.constCast<ScalarFunctionExpression>().getBindData();
    return dynamic_cast<const function::CastFunctionBindData*>(bindData) != nullptr &&
           LogicalTypeUtils::isIntegral(expression.getDataType()) &&
           LogicalTypeUtils::isIntegral(expression.getChild(0)->getDataType());
}

// The variable a key is, under integer casts: `{v: i}` on an INT8 column binds as
// `CAST_TO_INT8(i)`. A key computed from a variable (`i % 5`, `r.id`, a rounding cast) leaves the
// variable as payload, since rows with the same key can differ in it.
static const Expression* getKeyVariable(const Expression& key) {
    auto expression = &key;
    while (isIntegerCast(*expression)) {
        expression = expression->getChild(0).get();
    }
    return expression->expressionType == ExpressionType::VARIABLE ? expression : nullptr;
}

static bool hasNonKeyPayload(const Schema& schema, const expression_vector& keys,
    const std::vector<LogicalInsertInfo>& insertNodeInfos,
    const std::vector<LogicalInsertInfo>& insertRelInfos, const Expression& existenceMark) {
    std::unordered_set<std::string> keyNames;
    keyNames.insert(existenceMark.getUniqueName());
    for (auto& key : keys) {
        keyNames.insert(key->getUniqueName());
        if (auto variable = getKeyVariable(*key)) {
            keyNames.insert(variable->getUniqueName());
        }
    }
    for (auto& info : insertNodeInfos) {
        keyNames.insert(info.pattern->getUniqueName());
        for (auto& expression : info.columnExprs) {
            keyNames.insert(expression->getUniqueName());
        }
    }
    for (auto& info : insertRelInfos) {
        for (auto& expression : info.columnExprs) {
            keyNames.insert(expression->getUniqueName());
        }
    }
    for (auto& expression : schema.getExpressionsInScope()) {
        auto uniqueName = expression->getUniqueName();
        auto isMergePatternExpression = false;
        for (auto& info : insertNodeInfos) {
            isMergePatternExpression |= uniqueName.starts_with(info.pattern->getUniqueName() + ".");
        }
        for (auto& info : insertRelInfos) {
            isMergePatternExpression |= uniqueName.starts_with(info.pattern->getUniqueName() + ".");
        }
        if (!keyNames.contains(uniqueName) && !isMergePatternExpression) {
            return true;
        }
    }
    return false;
}

void LogicalMerge::computeSuppressDuplicateCreatedOutput() {
    auto childSchema = children[0]->getSchema();
    auto storesInsertedPatternIDs = onMatchSetNodeInfos.empty() && onMatchSetRelInfos.empty();
    suppressDuplicateCreatedOutput =
        storesInsertedPatternIDs && insertRelInfos.empty() && onCreateSetNodeInfos.empty() &&
        onCreateSetRelInfos.empty() &&
        !hasNonKeyPayload(*childSchema, keys, insertNodeInfos, insertRelInfos, *existenceMark);
}

void LogicalMerge::computeFactorizedSchema() {
    computeSuppressDuplicateCreatedOutput();
    copyChildSchema(0);
    for (auto& info : insertNodeInfos) {
        // Predicate iri is not matched but needs to be inserted.
        auto node = dynamic_cast_checked<NodeExpression*>(info.pattern.get());
        if (!schema->isExpressionInScope(*node->getInternalID())) {
            auto groupPos = schema->createGroup();
            schema->setGroupAsSingleState(groupPos);
            schema->insertToGroupAndScope(node->getInternalID(), groupPos);
        }
    }
    for (auto& info : insertRelInfos) {
        auto rel = dynamic_cast_checked<RelExpression*>(info.pattern.get());
        if (!schema->isExpressionInScope(*rel->getInternalID())) {
            auto groupPos = schema->createGroup();
            schema->setGroupAsSingleState(groupPos);
            schema->insertToGroupAndScope(rel->getInternalID(), groupPos);
        }
    }
}

void LogicalMerge::computeFlatSchema() {
    computeSuppressDuplicateCreatedOutput();
    copyChildSchema(0);
    for (auto& info : insertNodeInfos) {
        auto node = dynamic_cast_checked<NodeExpression*>(info.pattern.get());
        schema->insertToGroupAndScopeMayRepeat(node->getInternalID(), 0);
    }
    for (auto& info : insertRelInfos) {
        auto rel = dynamic_cast_checked<RelExpression*>(info.pattern.get());
        schema->insertToGroupAndScopeMayRepeat(rel->getInternalID(), 0);
    }
}

f_group_pos_set LogicalMerge::getGroupsPosToFlatten() {
    auto childSchema = children[0]->getSchema();
    return FlattenAll::getGroupsPosToFlatten(childSchema->getGroupsPosInScope(), *childSchema);
}

std::unique_ptr<LogicalOperator> LogicalMerge::copy() {
    auto merge = std::make_unique<LogicalMerge>(existenceMark, keys, children[0]->copy());
    merge->insertNodeInfos = copyVector(insertNodeInfos);
    merge->insertRelInfos = copyVector(insertRelInfos);
    merge->onCreateSetNodeInfos = copyVector(onCreateSetNodeInfos);
    merge->onCreateSetRelInfos = copyVector(onCreateSetRelInfos);
    merge->onMatchSetNodeInfos = copyVector(onMatchSetNodeInfos);
    merge->onMatchSetRelInfos = copyVector(onMatchSetRelInfos);
    merge->suppressDuplicateCreatedOutput = suppressDuplicateCreatedOutput;
    return merge;
}

} // namespace planner
} // namespace lbug

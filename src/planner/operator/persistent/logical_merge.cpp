#include "planner/operator/persistent/logical_merge.h"

#include <unordered_set>

#include "binder/expression/node_expression.h"
#include "binder/expression/rel_expression.h"
#include "binder/expression_visitor.h"
#include "common/cast.h"
#include "planner/operator/factorization/flatten_resolver.h"

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace planner {

static bool hasNonKeyPayload(const Schema& schema, const expression_vector& keys,
    const std::vector<LogicalInsertInfo>& insertNodeInfos,
    const std::vector<LogicalInsertInfo>& insertRelInfos, const Expression& existenceMark) {
    std::unordered_set<std::string> keyNames;
    keyNames.insert(existenceMark.getUniqueName());
    for (auto& key : keys) {
        keyNames.insert(key->getUniqueName());
        DependentVarNameCollector collector;
        collector.visit(key);
        for (auto& name : collector.getVarNames()) {
            keyNames.insert(name);
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

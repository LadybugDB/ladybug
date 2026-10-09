#include <algorithm>
#include <unordered_set>

#include "binder/binder.h"
#include "binder/expression/aggregate_function_expression.h"
#include "binder/expression/expression_util.h"
#include "binder/expression/node_rel_expression.h"
#include "binder/expression/property_expression.h"
#include "binder/expression/scalar_function_expression.h"
#include "binder/expression/subquery_expression.h"
#include "binder/expression_binder.h"
#include "binder/expression_visitor.h"
#include "common/exception/runtime.h"
#include "function/aggregate/count.h"
#include "function/list/vector_list_functions.h"
#include "main/client_context.h"
#include "planner/operator/factorization/flatten_resolver.h"
#include "planner/operator/logical_accumulate.h"
#include "planner/operator/logical_aggregate.h"
#include "planner/operator/logical_distinct.h"
#include "planner/operator/logical_flatten.h"
#include "planner/operator/logical_hash_join.h"
#include "planner/operator/logical_projection.h"
#include "planner/operator/logical_unwind.h"
#include "planner/operator/scan/logical_expressions_scan.h"
#include "planner/operator/scan/logical_query_primary_key_lookup.h"
#include "planner/planner.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace planner {

static expression_vector getDependentExprs(std::shared_ptr<Expression> expr, const Schema& schema) {
    auto analyzer = GroupDependencyAnalyzer(true /* collectDependentExpr */, schema);
    analyzer.visit(expr);
    return analyzer.getDependentExprs();
}

static bool isNodePrimaryKey(const Expression& expression, const NodeExpression& node,
    table_id_t tableID) {
    if (expression.expressionType != ExpressionType::PROPERTY) {
        return false;
    }
    auto& property = expression.constCast<PropertyExpression>();
    return property.getVariableName() == node.getInternalID()->getVariableName() &&
           property.isPrimaryKey(tableID);
}

// Correlated variant of tryPlanQueryPrimaryKeyLookup for OPTIONAL MATCH: when the optional
// clause matches a single node by primary key against outer expressions (e.g.
// OPTIONAL MATCH (p:Post {ID: msgId})), plan the right side as an expressions scan of the
// correlated bindings feeding a keyed PK lookup, then LEFT-hash-join as usual. PK
// uniqueness guarantees at most one match per outer row, so this preserves Left Join
// semantics exactly while avoiding a full node-table scan (which the generic correlated
// path would build). Returns false (caller falls back) when inapplicable.
bool Planner::findCorrelatedPrimaryKeyLookupKey(const QueryGraphCollection& queryGraphCollection,
    const expression_vector& predicates, const expression_vector& corrExprs,
    const Schema& outerSchema, std::shared_ptr<Expression>& key,
    expression_vector& residualPredicates) {
    key = nullptr;
    residualPredicates.clear();
    if (queryGraphCollection.getNumQueryGraphs() != 1) {
        return false;
    }
    auto queryGraph = queryGraphCollection.getQueryGraph(0);
    if (queryGraph->getNumQueryNodes() != 1 || queryGraph->getNumQueryRels() != 0) {
        return false;
    }
    auto node = queryGraph->getQueryNode(0);
    auto tableIDs = node->getTableIDs();
    if (tableIDs.size() != 1) {
        return false;
    }
    auto tableID = tableIDs[0];
    auto table = storage::StorageManager::Get(*clientContext)
                     ->getTable(tableID)
                     ->ptrCast<storage::NodeTable>();
    if (table->tryGetPrimaryKeyIndex() == nullptr) {
        return false;
    }
    std::unordered_set<std::string> corrNames;
    for (auto& expr : corrExprs) {
        corrNames.insert(expr->getUniqueName());
    }
    for (auto& predicate : predicates) {
        if (predicate->expressionType != ExpressionType::EQUALS) {
            residualPredicates.push_back(predicate);
            continue;
        }
        auto lhs = predicate->getChild(0);
        auto rhs = predicate->getChild(1);
        if (isNodePrimaryKey(*rhs, *node, tableID)) {
            std::swap(lhs, rhs);
        }
        // The key must be computable from the correlated bindings alone: every outer
        // expression it depends on has to be part of the expressions scan below.
        // A constant key (no outer dependents) never qualifies, so this check cannot
        // steal legs the unnest branch serves.
        auto usableKey = false;
        if (key == nullptr && isNodePrimaryKey(*lhs, *node, tableID)) {
            usableKey = true;
            for (auto& dep : getDependentExprs(rhs, outerSchema)) {
                if (!corrNames.contains(dep->getUniqueName())) {
                    usableKey = false;
                    break;
                }
            }
            if (getDependentExprs(rhs, outerSchema).empty()) {
                usableKey = false;
            }
        }
        if (!usableKey) {
            residualPredicates.push_back(predicate);
            continue;
        }
        key = rhs;
    }
    if (key == nullptr) {
        return false;
    }
    return true;
}

bool Planner::tryPlanCorrelatedPrimaryKeyLookup(const QueryGraphCollection& queryGraphCollection,
    const expression_vector& predicates, const expression_vector& corrExprs,
    const Schema& outerSchema, cardinality_t corrExprsCard, LogicalPlan& rightPlan) {
    if (!clientContext->getClientConfig()->enablePlanOptimizer) {
        return false;
    }
    std::shared_ptr<Expression> key;
    expression_vector residualPredicates;
    if (!findCorrelatedPrimaryKeyLookupKey(queryGraphCollection, predicates, corrExprs, outerSchema,
            key, residualPredicates)) {
        return false;
    }
    // Re-derive the validated single node/table (infallible: the finder above checked
    // this exact shape).
    auto node = queryGraphCollection.getQueryGraph(0)->getQueryNode(0);
    auto tableID = node->getTableIDs()[0];
    appendExpressionsScan(corrExprs, rightPlan);
    rightPlan.getLastOperator()->setCardinality(corrExprsCard);
    appendDistinct(corrExprs, rightPlan);
    appendFlattens(rightPlan.getSchema()->getGroupsPosInScope(), rightPlan);
    auto properties = getProperties(*node);
    properties.erase(std::remove_if(properties.begin(), properties.end(),
                         [](const std::shared_ptr<Expression>& expression) {
                             return expression->constCast<PropertyExpression>().isInternalID();
                         }),
        properties.end());
    const auto dependentExprs = getDependentExprs(key, *rightPlan.getSchema());
    DASSERT(!dependentExprs.empty());
    const auto outputGroupPos = rightPlan.getSchema()->getGroupPos(*dependentExprs[0]);
    for ([[maybe_unused]] auto& dependentExpr : dependentExprs) {
        DASSERT(rightPlan.getSchema()->getGroupPos(*dependentExpr) == outputGroupPos);
    }
    auto lookup = std::make_shared<LogicalQueryPrimaryKeyLookup>(tableID, node->getInternalID(),
        properties, key, outputGroupPos, rightPlan.getLastOperator());
    lookup->computeFactorizedSchema();
    lookup->setCardinality(rightPlan.getCardinality());
    rightPlan.setLastOperator(std::move(lookup));
    appendFilters(residualPredicates, rightPlan);
    return true;
}

bool Planner::tryPlanQueryPrimaryKeyLookup(const QueryGraphCollection& queryGraphCollection,
    const expression_vector& predicates, LogicalPlan& plan) {
    if (queryGraphCollection.getNumQueryGraphs() != 1) {
        return false;
    }
    auto queryGraph = queryGraphCollection.getQueryGraph(0);
    if (queryGraph->getNumQueryNodes() != 1 || queryGraph->getNumQueryRels() != 0) {
        return false;
    }
    auto node = queryGraph->getQueryNode(0);
    auto tableIDs = node->getTableIDs();
    if (tableIDs.size() != 1 || plan.getSchema()->isExpressionInScope(*node->getInternalID())) {
        return false;
    }
    auto tableID = tableIDs[0];
    auto table = storage::StorageManager::Get(*clientContext)
                     ->getTable(tableID)
                     ->ptrCast<storage::NodeTable>();
    if (table->tryGetPrimaryKeyIndex() == nullptr) {
        return false;
    }

    std::shared_ptr<Expression> key;
    expression_vector residualPredicates;
    for (auto& predicate : predicates) {
        if (predicate->expressionType != ExpressionType::EQUALS) {
            residualPredicates.push_back(predicate);
            continue;
        }
        auto lhs = predicate->getChild(0);
        auto rhs = predicate->getChild(1);
        if (isNodePrimaryKey(*rhs, *node, tableID)) {
            std::swap(lhs, rhs);
        }
        if (key == nullptr && isNodePrimaryKey(*lhs, *node, tableID) &&
            plan.getSchema()->evaluable(*rhs) &&
            !getDependentExprs(rhs, *plan.getSchema()).empty()) {
            key = rhs;
        } else {
            residualPredicates.push_back(predicate);
        }
    }
    if (key == nullptr) {
        return false;
    }

    appendFlattens(plan.getSchema()->getGroupsPosInScope(), plan);
    auto properties = getProperties(*node);
    properties.erase(std::remove_if(properties.begin(), properties.end(),
                         [](const std::shared_ptr<Expression>& expression) {
                             return expression->constCast<PropertyExpression>().isInternalID();
                         }),
        properties.end());
    const auto dependentExprs = getDependentExprs(key, *plan.getSchema());
    DASSERT(!dependentExprs.empty());
    const auto outputGroupPos = plan.getSchema()->getGroupPos(*dependentExprs[0]);
    for ([[maybe_unused]] auto& dependentExpr : dependentExprs) {
        DASSERT(plan.getSchema()->getGroupPos(*dependentExpr) == outputGroupPos);
    }
    auto lookup = std::make_shared<LogicalQueryPrimaryKeyLookup>(tableID, node->getInternalID(),
        properties, key, outputGroupPos, plan.getLastOperator());
    lookup->computeFactorizedSchema();
    lookup->setCardinality(plan.getCardinality());
    plan.setLastOperator(std::move(lookup));
    appendFilters(residualPredicates, plan);
    return true;
}

expression_vector Planner::getCorrelatedExprs(const QueryGraphCollection& collection,
    const expression_vector& predicates, Schema* outerSchema) {
    expression_vector result;
    for (auto& predicate : predicates) {
        for (auto& expression : getDependentExprs(predicate, *outerSchema)) {
            result.push_back(expression);
        }
    }
    for (auto& node : collection.getQueryNodes()) {
        if (outerSchema->isExpressionInScope(*node->getInternalID())) {
            result.push_back(node->getInternalID());
        }
    }
    return ExpressionUtil::removeDuplication(result);
}

// An equality with a constant (variable-free) side, e.g. a re-stated outer filter such
// as a.ID = 123 inside OPTIONAL MATCH, is a filter rather than a correlated join
// condition: the literal side can never serve as an unnestable join key. Treating it as
// correlated fails analysis and needlessly blocks unnesting (forcing expression-scan +
// full-scan plans). Keep it as a filter inside the subplan instead.
// The variable side must reference the inner query graph: a constant predicate over a
// variable that does not occur inside (e.g. an outer-only filter visible to an EXISTS
// subquery) cannot be applied there and must stay correlated, otherwise it is silently
// dropped and unnesting produces wrong results.
static bool isRoutableConstantEquality(const std::shared_ptr<Expression>& predicate,
    const binder::QueryGraphCollection& collection) {
    if (predicate->expressionType != common::ExpressionType::EQUALS) {
        return false;
    }
    std::unordered_set<std::string> innerNames;
    for (auto& node : collection.getQueryNodes()) {
        innerNames.insert(node->getUniqueName());
    }
    for (auto& rel : collection.getQueryRels()) {
        innerNames.insert(rel->getUniqueName());
    }
    bool hasConstantSide = false;
    for (auto i = 0u; i < 2u; ++i) {
        auto collector = DependentVarNameCollector();
        collector.visit(predicate->getChild(i));
        if (collector.getVarNames().empty()) {
            hasConstantSide = true;
            continue;
        }
        for (auto& varName : collector.getVarNames()) {
            if (!innerNames.contains(varName)) {
                return false;
            }
        }
    }
    return hasConstantSide;
}

class SubqueryPredicatePullUpAnalyzer {
public:
    // When routeConstantEqualities is false, constant-equality predicates keep the legacy
    // treatment (correlated), so analysis behaves exactly as before the unnesting change.
    // Used for MERGE existence checks and explicit join hints (see planOptionalMatch).
    SubqueryPredicatePullUpAnalyzer(const Schema& schema,
        const QueryGraphCollection& queryGraphCollection, bool routeConstantEqualities = true)
        : schema{schema}, queryGraphCollection{queryGraphCollection},
          routeConstantEqualities{routeConstantEqualities} {}

    bool analyze(const expression_vector& predicates) {
        expression_vector correlatedPredicates;
        for (auto& predicate : predicates) {
            if (getDependentExprs(predicate, schema).empty() ||
                (routeConstantEqualities &&
                    isRoutableConstantEquality(predicate, queryGraphCollection))) {
                nonCorrelatedPredicates.push_back(predicate);
            } else {
                correlatedPredicates.push_back(predicate);
            }
        }
        for (auto predicate : correlatedPredicates) {
            auto [left, right] = analyze(predicate);
            if (left == nullptr) {
                return false;
            }
            joinConditions.emplace_back(left, right);
        }
        for (auto& node : queryGraphCollection.getQueryNodes()) {
            if (schema.isExpressionInScope(*node->getInternalID())) {
                joinConditions.emplace_back(node->getInternalID(), node->getInternalID());
            }
        }
        return true;
    }

    expression_vector getNonCorrelatedPredicates() const { return nonCorrelatedPredicates; }
    std::vector<binder::expression_pair> getJoinConditions() const { return joinConditions; }

    expression_vector getCorrelatedInternalIDs() const {
        expression_vector exprs;
        for (auto& node : queryGraphCollection.getQueryNodes()) {
            if (schema.isExpressionInScope(*node->getInternalID())) {
                exprs.push_back(node->getInternalID());
            }
        }
        return exprs;
    }

private:
    expression_pair analyze(std::shared_ptr<Expression> predicate) {
        if (predicate->expressionType != common::ExpressionType::EQUALS) {
            return {nullptr, nullptr};
        }
        auto left = predicate->getChild(0);
        auto right = predicate->getChild(1);
        if (isUnnestableJoinCondition(*left, *right)) {
            return {left, right};
        }
        if (isUnnestableJoinCondition(*right, *left)) {
            return {right, left};
        }
        return {nullptr, nullptr};
    }

    bool isUnnestableJoinCondition(const Expression& left, const Expression& right) {
        return right.expressionType == ExpressionType::PROPERTY &&
               schema.isExpressionInScope(left) && !schema.isExpressionInScope(right);
    }

private:
    const Schema& schema;
    const QueryGraphCollection& queryGraphCollection;
    bool routeConstantEqualities;

    expression_vector nonCorrelatedPredicates;
    std::vector<binder::expression_pair> joinConditions;
};

void Planner::planOptionalMatch(const QueryGraphCollection& queryGraphCollection,
    const expression_vector& predicates, LogicalPlan& leftPlan,
    std::shared_ptr<BoundJoinHintNode> hint) {
    planOptionalMatch(queryGraphCollection, predicates, nullptr /* mark */, leftPlan,
        std::move(hint));
}

// ---- Collect-membership unnest: WITH k, COLLECT(x) AS C ... WHERE y IN C ----
// Rewrites an IN-membership test over an outer COLLECT-built node list (y bound
// inside the OPTIONAL MATCH leg) into (group key, element) rows plus an equality
// predicate: WITH DISTINCT k, x._ID plus y._ID = x._ID. The list form forces per-row
// list scans/copies/hashes; the row form hash-joins instead. Sound when every collect
// group is non-empty (collect input is inner, so groups form from actual rows and x
// is non-null) and the rows reach the leg without fanning out (only
// pass-through/reducing ops between). Implemented append-natively: UNWIND (with
// idExpr, so x._ID is materialized into scope like a bound node-list UNWIND) +
// DISTINCT are appended to the outer plan (schemas maintained by the append helpers)
// and the predicate is rebuilt with the expression binder, so downstream planning
// (including unnest analysis, which turns the equality into a hash-join key) proceeds
// unchanged.

static bool isListContainsFunc(const std::shared_ptr<Expression>& pred) {
    if (pred->expressionType != ExpressionType::FUNCTION) {
        return false;
    }
    auto& funcExpr = pred->constCast<ScalarFunctionExpression>();
    auto name = funcExpr.getFunction().name;
    return name == function::ListContainsFunction::name || name == function::ListHasFunction::name;
}

static bool containsRef(const std::shared_ptr<Expression>& expr, const std::string& uniqueName) {
    if (expr->getUniqueName() == uniqueName) {
        return true;
    }
    for (auto& child : expr->getChildren()) {
        if (containsRef(child, uniqueName)) {
            return true;
        }
    }
    return false;
}

static bool containsSubqueryOrLambda(const std::shared_ptr<Expression>& expr) {
    if (expr->expressionType == ExpressionType::SUBQUERY ||
        expr->expressionType == ExpressionType::LAMBDA) {
        return true;
    }
    for (auto& child : expr->getChildren()) {
        if (containsSubqueryOrLambda(child)) {
            return true;
        }
    }
    return false;
}

// Top-down search for the nearest AGGREGATE producing the collect variable (by unique
// name) via a single-arg COLLECT. Read-only; other aggregates in the same op ride along
// as payloads. Returns the op, or nullptr.
static planner::LogicalOperator* findCollectAggregate(planner::LogicalOperator* op,
    const std::string& collectVarName, std::shared_ptr<Expression>& collectArg,
    expression_vector& groupKeys) {
    if (op->getOperatorType() == LogicalOperatorType::AGGREGATE) {
        auto& aggregate = op->constCast<LogicalAggregate>();
        for (auto& aggExpr : aggregate.getAggregates()) {
            if (aggExpr->expressionType != ExpressionType::AGGREGATE_FUNCTION) {
                continue;
            }
            auto& aggFunc = aggExpr->constCast<AggregateFunctionExpression>();
            if (aggFunc.getFunction().name != function::CollectFunction::name ||
                aggFunc.getNumChildren() != 1) {
                continue;
            }
            if (aggExpr->getUniqueName() == collectVarName) {
                collectArg = aggFunc.getChild(0);
                groupKeys = aggregate.getKeys();
                return groupKeys.empty() ? nullptr : op;
            }
        }
    }
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        if (auto found = findCollectAggregate(op->getChild(i).get(), collectVarName, collectArg,
                groupKeys)) {
            return found;
        }
    }
    return nullptr;
}

// See the declaration in planner.h: every enumerator is classified explicitly with no
// default label, so -Wswitch fails the build on unclassified new operators (#935).
// The true arm holds operators that only pass through, filter, or combine already-bound
// rows and therefore cannot introduce null bindings for the collect element. Everything
// else (UNION_ALL, UNWIND, table functions, write/DDL ops, ...) conservatively counts
// as potentially null-supplying: the rewrite is skipped (missed optimization, never
// wrong results).
bool isNullFreeOperator(LogicalOperatorType type) {
    switch (type) {
    case LogicalOperatorType::AGGREGATE:
    case LogicalOperatorType::COUNT_ANTI_EDGE_CHAIN:
    case LogicalOperatorType::COUNT_EXTEND_CHAIN:
    case LogicalOperatorType::COUNT_REL_TABLE:
    case LogicalOperatorType::CROSS_PRODUCT:
    case LogicalOperatorType::DISTINCT:
    case LogicalOperatorType::DUMMY_SCAN:
    case LogicalOperatorType::EMPTY_RESULT:
    case LogicalOperatorType::EXTEND:
    case LogicalOperatorType::FILTER:
    case LogicalOperatorType::FLATTEN:
    case LogicalOperatorType::INDEX_LOOK_UP:
    case LogicalOperatorType::INTERSECT:
    case LogicalOperatorType::LIMIT:
    case LogicalOperatorType::MULTIPLICITY_REDUCER:
    case LogicalOperatorType::NODE_LABEL_FILTER:
    case LogicalOperatorType::ORDER_BY:
    case LogicalOperatorType::PACKED_EXTEND:
    case LogicalOperatorType::PATH_PROPERTY_PROBE:
    case LogicalOperatorType::PROJECTION:
    case LogicalOperatorType::QUERY_PRIMARY_KEY_LOOKUP:
    case LogicalOperatorType::REACHABLE_COUNT:
    case LogicalOperatorType::GROUPED_REACHABLE_COUNT:
    case LogicalOperatorType::RECURSIVE_EXTEND:
    case LogicalOperatorType::REL_DEGREE_TABLE:
    case LogicalOperatorType::SCAN_NODE_TABLE:
    case LogicalOperatorType::SEMI_MASKER:
        return true;
    case LogicalOperatorType::ACCUMULATE:
    case LogicalOperatorType::ALTER:
    case LogicalOperatorType::ANALYZE:
    case LogicalOperatorType::ATTACH_DATABASE:
    case LogicalOperatorType::COPY_FROM:
    case LogicalOperatorType::COPY_TO:
    case LogicalOperatorType::CREATE_GRAPH:
    case LogicalOperatorType::CREATE_INDEX:
    case LogicalOperatorType::CREATE_MACRO:
    case LogicalOperatorType::CREATE_SEQUENCE:
    case LogicalOperatorType::CREATE_TABLE:
    case LogicalOperatorType::CREATE_TYPE:
    case LogicalOperatorType::DELETE:
    case LogicalOperatorType::DETACH_DATABASE:
    case LogicalOperatorType::DROP:
    case LogicalOperatorType::DUMMY_SINK:
    case LogicalOperatorType::EXPLAIN:
    case LogicalOperatorType::EXPRESSIONS_SCAN:
    case LogicalOperatorType::EXTENSION:
    case LogicalOperatorType::EXTENSION_CLAUSE:
    case LogicalOperatorType::EXPORT_DATABASE:
    case LogicalOperatorType::HASH_JOIN:
    case LogicalOperatorType::IMPORT_DATABASE:
    case LogicalOperatorType::INSERT:
    case LogicalOperatorType::MERGE:
    case LogicalOperatorType::NOOP:
    case LogicalOperatorType::PARTITIONER:
    case LogicalOperatorType::SET_PROPERTY:
    case LogicalOperatorType::STANDALONE_CALL:
    case LogicalOperatorType::TABLE_FUNCTION_CALL:
    case LogicalOperatorType::TRANSACTION:
    case LogicalOperatorType::UNION_ALL:
    case LogicalOperatorType::UNWIND:
    case LogicalOperatorType::UNWIND_DEDUPLICATE:
    case LogicalOperatorType::USE_DATABASE:
    case LogicalOperatorType::USE_GRAPH:
        return false;
    }
    // No default label: -Wswitch enforces that every enumerator is handled above.
    // LCOV_EXCL_START
    throw common::RuntimeException("Unknown logical operator type.");
    // LCOV_EXCL_STOP
}

// True when the subtree can supply nulls (which would break the non-empty-group /
// non-null-element reasoning): LEFT/MARK joins, non-regular accumulates, and anything
// not explicitly classified null-free above. Errs toward skipping the rewrite.
static bool subtreeHasNullSupply(planner::LogicalOperator* op) {
    switch (op->getOperatorType()) {
    case LogicalOperatorType::HASH_JOIN: {
        auto joinType = op->constCast<LogicalHashJoin>().getJoinType();
        if (joinType == JoinType::LEFT || joinType == JoinType::MARK) {
            return true;
        }
        break;
    }
    case LogicalOperatorType::ACCUMULATE: {
        if (op->constCast<LogicalAccumulate>().getAccumulateType() != AccumulateType::REGULAR) {
            return true;
        }
        break;
    }
    default:
        if (!isNullFreeOperator(op->getOperatorType())) {
            return true;
        }
        break;
    }
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        if (subtreeHasNullSupply(op->getChild(i).get())) {
            return true;
        }
    }
    return false;
}

// Collects the operator path from root down to (excluding) target. Returns false when
// target is not under root.
static bool findPath(planner::LogicalOperator* op, planner::LogicalOperator* target,
    std::vector<planner::LogicalOperator*>& path) {
    if (op == target) {
        return true;
    }
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        if (findPath(op->getChild(i).get(), target, path)) {
            path.push_back(op);
            return true;
        }
    }
    return false;
}

// True when every op on the path preserves row count (no fan-out), so the
// (group key, element) rows formed at the top correspond 1:1 to collect groups
// modulo reduction.
static bool pathHasFanOut(const std::vector<planner::LogicalOperator*>& path) {
    for (auto* op : path) {
        switch (op->getOperatorType()) {
        case LogicalOperatorType::PROJECTION:
        case LogicalOperatorType::FILTER:
        case LogicalOperatorType::FLATTEN:
        case LogicalOperatorType::DISTINCT:
        case LogicalOperatorType::ORDER_BY:
        case LogicalOperatorType::LIMIT:
        case LogicalOperatorType::INTERSECT:
        case LogicalOperatorType::SEMI_MASKER:
        case LogicalOperatorType::NODE_LABEL_FILTER:
        case LogicalOperatorType::MULTIPLICITY_REDUCER:
            break;
        case LogicalOperatorType::ACCUMULATE:
            if (op->constCast<LogicalAccumulate>().getAccumulateType() != AccumulateType::REGULAR) {
                return true;
            }
            break;
        default:
            return true;
        }
    }
    return false;
}

static std::shared_ptr<Expression> findInScope(planner::Schema& schema,
    const std::string& uniqueName) {
    for (auto& expr : schema.getExpressionsInScope()) {
        if (expr->getUniqueName() == uniqueName) {
            return expr;
        }
    }
    return nullptr;
}

static std::unordered_set<std::string> collectOuterScopeNames(planner::Schema& schema) {
    std::unordered_set<std::string> names;
    for (auto& expr : schema.getExpressionsInScope()) {
        names.insert(expr->getUniqueName());
    }
    return names;
}

static std::unordered_set<std::string> collectGraphVarNames(
    const QueryGraphCollection& collection) {
    std::unordered_set<std::string> names;
    for (auto& node : collection.getQueryNodes()) {
        names.insert(node->getUniqueName());
    }
    for (auto& rel : collection.getQueryRels()) {
        names.insert(rel->getUniqueName());
    }
    return names;
}

bool Planner::tryUnnestCollectMembership(const QueryGraphCollection& queryGraphCollection,
    expression_vector& predicates, LogicalPlan& leftPlan) {
    // Fast path: no list-membership predicate, nothing to do.
    bool anyContains = false;
    for (auto& pred : predicates) {
        if (isListContainsFunc(pred)) {
            anyContains = true;
            break;
        }
    }
    if (!anyContains || leftPlan.isEmpty()) {
        return false;
    }
    auto outerScope = collectOuterScopeNames(*leftPlan.getSchema());
    auto legVars = collectGraphVarNames(queryGraphCollection);
    struct Convertible {
        size_t predIdx;
        std::shared_ptr<Expression> elemNode;
    };
    std::vector<Convertible> convertibles;
    std::string collectName;
    for (auto i = 0u; i < predicates.size(); ++i) {
        auto& pred = predicates[i];
        if (!isListContainsFunc(pred) || pred->getNumChildren() != 2) {
            continue;
        }
        // Determine roles order-agnostically: the list side lives in the outer scope,
        // the element side is a leg-local node/rel pattern (bound by the leg's MATCH).
        auto c0 = pred->getChild(0);
        auto c1 = pred->getChild(1);
        auto c0Outer = outerScope.contains(c0->getUniqueName());
        auto c1Outer = outerScope.contains(c1->getUniqueName());
        std::shared_ptr<Expression> listObj;
        std::shared_ptr<Expression> elemObj;
        if (c0Outer && !c1Outer) {
            listObj = c0;
            elemObj = c1;
        } else if (c1Outer && !c0Outer) {
            listObj = c1;
            elemObj = c0;
        } else {
            return false;
        }
        if (!ExpressionUtil::isNodePattern(*elemObj) && !ExpressionUtil::isRelPattern(*elemObj)) {
            return false;
        }
        if (!legVars.contains(elemObj->getUniqueName()) ||
            legVars.contains(listObj->getUniqueName())) {
            return false;
        }
        if (collectName.empty()) {
            collectName = listObj->getUniqueName();
        } else if (collectName != listObj->getUniqueName()) {
            return false;
        }
        convertibles.push_back({i, elemObj});
    }
    if (convertibles.empty()) {
        return false;
    }
    // Every remaining reference to the list must vanish with the rewrite: no other uses
    // (which would dangle once the list is dropped below), and no subqueries/lambdas
    // that could hide references from the tree walk.
    std::unordered_set<size_t> convertedIdxs;
    for (auto& conv : convertibles) {
        convertedIdxs.insert(conv.predIdx);
    }
    for (auto i = 0u; i < predicates.size(); ++i) {
        if (convertedIdxs.contains(i)) {
            continue;
        }
        if (containsSubqueryOrLambda(predicates[i]) || containsRef(predicates[i], collectName)) {
            return false;
        }
    }
    // Locate the producing aggregate and validate the shape: single-arg COLLECT over a
    // node pattern, non-empty group keys, and an inner (null-free) build subtree, so
    // every group yields at least one (group key, element) row. Rel elements are skipped:
    // the UNWIND below materializes the element's internal ID into its own vector, which
    // UNWIND only supports for node lists (and the binder likewise only sets an UNWIND
    // idExpr for node lists).
    std::shared_ptr<Expression> collectArg;
    expression_vector groupKeys;
    auto aggOp =
        findCollectAggregate(leftPlan.getLastOperator().get(), collectName, collectArg, groupKeys);
    if (aggOp == nullptr || collectArg == nullptr || !ExpressionUtil::isNodePattern(*collectArg)) {
        return false;
    }
    // The element's internal ID. The rewritten equality (node comparison, which the
    // binder lowers to an _ID comparison) references this object, so it must be a
    // first-class in-scope expression in the outer plan: that is what lets correlated
    // planning carry it into the leg (EXPRESSIONS_SCAN) and what lets the unnest
    // analyzer turn the equality into a hash-join key. A bare _ID property has no
    // expression children, so no analyzer can derive it from the element struct.
    if (subtreeHasNullSupply(aggOp)) {
        return false;
    }
    std::vector<planner::LogicalOperator*> path;
    if (!findPath(leftPlan.getLastOperator().get(), aggOp, path) || pathHasFanOut(path)) {
        return false;
    }
    // Resolve the (group key, element) components in the current (top) scope.
    auto& schema = *leftPlan.getSchema();
    auto listObj = findInScope(schema, collectName);
    if (listObj == nullptr) {
        return false;
    }
    expression_vector distinctKeys;
    for (auto& key : groupKeys) {
        auto scoped = findInScope(schema, key->getUniqueName());
        if (scoped == nullptr) {
            return false;
        }
        distinctKeys.push_back(scoped);
    }
    // Build the equality predicates first so leftPlan is untouched on failure.
    Binder binder(clientContext);
    ExpressionBinder expressionBinder(&binder, clientContext);
    expression_vector newPreds;
    for (auto& conv : convertibles) {
        newPreds.push_back(
            expressionBinder.createEqualityComparisonExpression(conv.elemNode, collectArg));
    }
    // Append UNWIND (list -> element rows, materializing the element ID vector via
    // the UNWIND idExpr exactly like a bound UNWIND of a node list) + DISTINCT
    // (deduplicated (group key, element) rows keyed by group keys + element ID; list
    // and element struct dropped, neither is needed downstream). The ID lands in the same
    // factorized group as the element with its own vector, so all downstream consumers map it as an
    // ordinary in-scope reference.
    auto elementID = collectArg->constCast<NodeExpression>().getInternalID();
    auto unwind =
        std::make_shared<LogicalUnwind>(listObj, collectArg, elementID, leftPlan.getLastOperator());
    appendFlattens(unwind->getGroupsPosToFlatten(), leftPlan);
    unwind->setChild(0, leftPlan.getLastOperator());
    unwind->computeFactorizedSchema();
    leftPlan.setLastOperator(unwind);
    expression_vector payloads;
    for (auto& expr : leftPlan.getSchema()->getExpressionsInScope()) {
        auto name = expr->getUniqueName();
        if (name == collectName || name == collectArg->getUniqueName()) {
            continue;
        }
        bool isKey = name == elementID->getUniqueName();
        for (auto& key : distinctKeys) {
            isKey = isKey || name == key->getUniqueName();
        }
        if (!isKey) {
            payloads.push_back(expr);
        }
    }
    distinctKeys.push_back(elementID);
    auto distinct = std::make_shared<LogicalDistinct>(distinctKeys, leftPlan.getLastOperator());
    distinct->setPayloads(std::move(payloads));
    appendFlattens(distinct->getGroupsPosToFlatten(), leftPlan);
    distinct->setChild(0, leftPlan.getLastOperator());
    distinct->computeFactorizedSchema();
    leftPlan.setLastOperator(distinct);
    for (auto i = 0u; i < convertibles.size(); ++i) {
        predicates[convertibles[i].predIdx] = newPreds[i];
    }
    return true;
}

// ---- Property collect-membership unnest with PK-seeded chain ----
// Shape: WITH ... COLLECT(n.Prop) AS list ... MATCH <linear chain> WHERE m.Prop IN list,
// where m.Prop is m's single-table primary key (e.g. LDBC SNB Q12's tag.ID IN tags).
// tryUnnestCollectMembership above only handles node/rel elements; a property element
// cannot seed the DP enumerator (correlation is node-ID-centric), so the leg is planned
// directly instead:
//   1. UNWIND(list AS freshElem)+DISTINCT on the outer plan. The element gets a FRESH
//      variable: the same `var.Prop` name may be bound in both parts, so reusing the
//      collect arg would collide scopes.
//   2. PK lookup of the seed node keyed by the unwound values (every ID comes from the
//      node's own table, so the lookup is exact and the IN predicate is consumed).
//   3. index-nested-loop extends along the linear chain away from the seed.
//   4. when the far end carries a constant PK equality (person anchor), stop one hop
//      short and meet the anchor side (PK scan + one extend, plus the meeting node's
//      properties) with an INNER hash join on the meeting node's ID instead of
//      expanding into the anchor: the expansion fans out (6k friends -> 260k person
//      rows) only to be filtered back to one person, while the meet keeps both sides
//      small and SIP-prunable.
// Fires only for a single-graph leg that is a simple path from the seed node, with the
// seed property the single-table PK (backed by a PK index), no predicate touching any
// leg variable except the consumed IN and the anchor PK, and all rels non-recursive.
// Anything else returns false and the caller falls back to regular planning.

static common::ExtendDirection chainExtendDirection(const RelExpression& rel,
    const NodeExpression& boundNode) {
    if (rel.getDirectionType() == binder::RelDirectionType::BOTH) {
        return common::ExtendDirection::BOTH;
    }
    return *rel.getSrcNode() == boundNode ? common::ExtendDirection::FWD :
                                            common::ExtendDirection::BWD;
}

// Top-down search for the nearest AGGREGATE producing the collect variable via a
// single-arg COLLECT. Unlike findCollectAggregate (which serves the node-element
// rewrite and requires group keys), a global (keyless) collect is accepted: unwinding
// it still yields exactly the collected values.
static planner::LogicalOperator* findPropertyCollectAggregate(planner::LogicalOperator* op,
    const std::string& collectVarName, std::shared_ptr<Expression>& collectArg,
    expression_vector& groupKeys) {
    if (op->getOperatorType() == LogicalOperatorType::AGGREGATE) {
        auto& aggregate = op->constCast<LogicalAggregate>();
        for (auto& aggExpr : aggregate.getAggregates()) {
            if (aggExpr->expressionType != ExpressionType::AGGREGATE_FUNCTION) {
                continue;
            }
            auto& aggFunc = aggExpr->constCast<AggregateFunctionExpression>();
            if (aggFunc.getFunction().name != function::CollectFunction::name ||
                aggFunc.getNumChildren() != 1) {
                continue;
            }
            if (aggExpr->getUniqueName() == collectVarName) {
                collectArg = aggFunc.getChild(0);
                groupKeys = aggregate.getKeys();
                return op;
            }
        }
    }
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        if (auto found = findPropertyCollectAggregate(op->getChild(i).get(), collectVarName,
                collectArg, groupKeys)) {
            return found;
        }
    }
    return nullptr;
}

static bool isVarFree(const std::shared_ptr<Expression>& expr) {
    auto collector = DependentVarNameCollector();
    collector.visit(expr);
    return collector.getVarNames().empty();
}

bool Planner::tryPlanPropertySeededChain(const QueryGraphCollection& queryGraphCollection,
    const expression_vector& predicates, LogicalPlan& leftPlan) {
    if (leftPlan.isEmpty() || leftPlan.hasUpdate() ||
        queryGraphCollection.getNumQueryGraphs() != 1) {
        return false;
    }
    auto queryGraph = queryGraphCollection.getQueryGraph(0);
    if (queryGraph->getNumQueryRels() == 0) {
        return false;
    }
    for (auto i = 0u; i < queryGraph->getNumQueryRels(); ++i) {
        if (queryGraph->getQueryRel(i)->getRelType() != QueryRelType::NON_RECURSIVE) {
            return false;
        }
    }
    // 1. Find the single convertible IN predicate: outer-scope list, leg-local
    // single-label property element.
    auto outerScope = collectOuterScopeNames(*leftPlan.getSchema());
    auto legVars = collectGraphVarNames(queryGraphCollection);
    int convertIdx = -1;
    std::shared_ptr<Expression> listObj;
    std::shared_ptr<Expression> elemProp;
    for (auto i = 0u; i < predicates.size(); ++i) {
        auto& pred = predicates[i];
        if (!isListContainsFunc(pred) || pred->getNumChildren() != 2) {
            continue;
        }
        auto c0 = pred->getChild(0);
        auto c1 = pred->getChild(1);
        auto c0Outer = outerScope.contains(c0->getUniqueName());
        auto c1Outer = outerScope.contains(c1->getUniqueName());
        std::shared_ptr<Expression> listSide;
        std::shared_ptr<Expression> elemSide;
        if (c0Outer && !c1Outer) {
            listSide = c0;
            elemSide = c1;
        } else if (c1Outer && !c0Outer) {
            listSide = c1;
            elemSide = c0;
        } else {
            return false;
        }
        if (elemSide->expressionType != ExpressionType::PROPERTY ||
            !elemSide->constCast<PropertyExpression>().isSingleLabel()) {
            return false;
        }
        auto& elemPropRef = elemSide->constCast<PropertyExpression>();
        if (!legVars.contains(elemPropRef.getVariableName()) ||
            legVars.contains(listSide->getUniqueName())) {
            return false;
        }
        if (convertIdx >= 0) {
            return false;
        }
        convertIdx = static_cast<int>(i);
        listObj = listSide;
        elemProp = elemSide;
    }
    if (convertIdx < 0) {
        return false;
    }
    auto collectName = listObj->getUniqueName();
    // Every other predicate must be free of hidden references; leg-variable touches are
    // validated below (only the anchor PK may remain).
    for (auto i = 0u; i < predicates.size(); ++i) {
        if (static_cast<int>(i) == convertIdx) {
            continue;
        }
        if (containsSubqueryOrLambda(predicates[i]) || containsRef(predicates[i], collectName)) {
            return false;
        }
    }
    // 2. Validate the producing COLLECT: single-arg over a property of the same
    // table/property, null-free subtree, no fan-out above it.
    std::shared_ptr<Expression> collectArg;
    expression_vector groupKeys;
    auto aggOp = findPropertyCollectAggregate(leftPlan.getLastOperator().get(), collectName,
        collectArg, groupKeys);
    if (aggOp == nullptr || collectArg == nullptr ||
        collectArg->expressionType != ExpressionType::PROPERTY ||
        !collectArg->constCast<PropertyExpression>().isSingleLabel()) {
        return false;
    }
    auto& collectProp = collectArg->constCast<PropertyExpression>();
    auto& elemPropRef = elemProp->constCast<PropertyExpression>();
    if (collectProp.getSingleTableID() != elemPropRef.getSingleTableID() ||
        collectProp.getPropertyName() != elemPropRef.getPropertyName()) {
        return false;
    }
    if (subtreeHasNullSupply(aggOp)) {
        return false;
    }
    std::vector<planner::LogicalOperator*> path;
    if (!findPath(leftPlan.getLastOperator().get(), aggOp, path) || pathHasFanOut(path)) {
        return false;
    }
    auto& schema = *leftPlan.getSchema();
    auto listScoped = findInScope(schema, collectName);
    if (listScoped == nullptr) {
        return false;
    }
    expression_vector distinctKeys;
    for (auto& key : groupKeys) {
        auto scoped = findInScope(schema, key->getUniqueName());
        if (scoped == nullptr) {
            return false;
        }
        distinctKeys.push_back(scoped);
    }
    // 3. Seed viability: the element's node, single table, PK with an index.
    std::shared_ptr<NodeExpression> seedNode;
    for (auto i = 0u; i < queryGraph->getNumQueryNodes(); ++i) {
        auto node = queryGraph->getQueryNode(i);
        if (node->getUniqueName() == elemPropRef.getVariableName()) {
            seedNode = node;
            break;
        }
    }
    if (seedNode == nullptr || seedNode->getTableIDs().size() != 1) {
        return false;
    }
    auto seedTableID = seedNode->getTableIDs()[0];
    if (!elemPropRef.isPrimaryKey(seedTableID)) {
        return false;
    }
    auto seedTable = storage::StorageManager::Get(*clientContext)
                         ->getTable(seedTableID)
                         ->ptrCast<storage::NodeTable>();
    if (seedTable->tryGetPrimaryKeyIndex() == nullptr) {
        return false;
    }
    // Anchor: a constant PK equality on another single-table node (at most one), and no
    // other predicate may touch any leg variable.
    std::shared_ptr<NodeExpression> anchorNode;
    int anchorPredIdx = -1;
    for (auto i = 0u; i < predicates.size(); ++i) {
        if (static_cast<int>(i) == convertIdx) {
            continue;
        }
        auto& pred = predicates[i];
        if (pred->expressionType != ExpressionType::EQUALS) {
            // Residual non-equality predicates are applied after the chain; they must not
            // touch leg variables (they cannot be placed mid-chain).
            if (!isVarFree(pred)) {
                auto collector = DependentVarNameCollector();
                collector.visit(pred);
                for (auto& var : collector.getVarNames()) {
                    if (legVars.contains(var)) {
                        return false;
                    }
                }
            }
            continue;
        }
        auto lhs = pred->getChild(0);
        auto rhs = pred->getChild(1);
        bool matched = false;
        for (auto n = 0u; n < queryGraph->getNumQueryNodes(); ++n) {
            auto node = queryGraph->getQueryNode(n);
            if (node->getTableIDs().size() != 1) {
                continue;
            }
            auto tableID = node->getTableIDs()[0];
            auto l = lhs;
            auto r = rhs;
            if (isNodePrimaryKey(*r, *node, tableID)) {
                std::swap(l, r);
            }
            if (isNodePrimaryKey(*l, *node, tableID) && isVarFree(r)) {
                if (anchorNode != nullptr || node->getUniqueName() == seedNode->getUniqueName()) {
                    return false;
                }
                anchorNode = node;
                anchorPredIdx = static_cast<int>(i);
                matched = true;
                break;
            }
        }
        if (!matched) {
            // A leg-variable-touching equality that is not the anchor disqualifies.
            auto collector = DependentVarNameCollector();
            collector.visit(pred);
            for (auto& var : collector.getVarNames()) {
                if (legVars.contains(var)) {
                    return false;
                }
            }
        }
    }
    // 4. Linearity walk from the seed. Stops at the anchor (meet mode) or at the path
    // end (full-walk mode, anchor-free only).
    std::unordered_map<std::string, std::shared_ptr<NodeExpression>> nodeByName;
    for (auto i = 0u; i < queryGraph->getNumQueryNodes(); ++i) {
        auto node = queryGraph->getQueryNode(i);
        nodeByName[node->getUniqueName()] = node;
    }
    struct ChainStep {
        std::shared_ptr<RelExpression> rel;
        std::shared_ptr<NodeExpression> nbr;
    };
    std::vector<ChainStep> steps;
    std::unordered_set<std::string> usedRels;
    std::shared_ptr<RelExpression> meetRel;
    auto meetNode = seedNode;
    auto cur = seedNode;
    auto numRels = queryGraph->getNumQueryRels();
    while (true) {
        std::vector<ChainStep> cands;
        for (auto i = 0u; i < numRels; ++i) {
            auto rel = queryGraph->getQueryRel(i);
            if (usedRels.contains(rel->getUniqueName())) {
                continue;
            }
            auto srcIsCur = *rel->getSrcNode() == *cur;
            auto dstIsCur = *rel->getDstNode() == *cur;
            if (srcIsCur && dstIsCur) {
                return false;
            }
            if (!srcIsCur && !dstIsCur) {
                continue;
            }
            auto nbr = srcIsCur ? rel->getDstNode() : rel->getSrcNode();
            cands.push_back({rel, nbr});
        }
        if (cands.size() != 1) {
            if (!cands.empty() || anchorNode != nullptr || usedRels.size() != numRels) {
                return false;
            }
            break;
        }
        auto stepRel = cands[0].rel;
        auto stepNbr = cands[0].nbr;
        if (anchorNode != nullptr && *stepNbr == *anchorNode) {
            meetRel = stepRel;
            meetNode = cur;
            break;
        }
        steps.push_back({stepRel, stepNbr});
        usedRels.insert(stepRel->getUniqueName());
        cur = stepNbr;
        if (steps.size() > numRels) {
            return false;
        }
    }
    if (meetRel != nullptr && usedRels.size() + 1 != numRels) {
        return false;
    }
    // 5. Mutate: UNWIND + DISTINCT, PK lookup, chain extends, anchor meet.
    // Hand-built scans bypass DP enumeration, which normally initializes the cardinality
    // estimator (planQueryGraph); initialize it for this leg explicitly.
    cardinalityEstimator.init(*queryGraph);
    Binder binder(clientContext);
    auto freshElem = std::make_shared<PropertyExpression>(elemPropRef);
    freshElem->setUniqueName(binder.getUniqueExpressionName(elemProp->getUniqueName()));
    auto unwind =
        std::make_shared<LogicalUnwind>(listScoped, freshElem, nullptr, leftPlan.getLastOperator());
    appendFlattens(unwind->getGroupsPosToFlatten(), leftPlan);
    unwind->setChild(0, leftPlan.getLastOperator());
    unwind->computeFactorizedSchema();
    leftPlan.setLastOperator(unwind);
    distinctKeys.push_back(freshElem);
    expression_vector payloads;
    for (auto& expr : leftPlan.getSchema()->getExpressionsInScope()) {
        auto name = expr->getUniqueName();
        if (name == collectName || name == freshElem->getUniqueName()) {
            continue;
        }
        bool isKey = false;
        for (auto& key : distinctKeys) {
            isKey = isKey || name == key->getUniqueName();
        }
        if (!isKey) {
            payloads.push_back(expr);
        }
    }
    auto distinct = std::make_shared<LogicalDistinct>(distinctKeys, leftPlan.getLastOperator());
    distinct->setPayloads(std::move(payloads));
    appendFlattens(distinct->getGroupsPosToFlatten(), leftPlan);
    distinct->setChild(0, leftPlan.getLastOperator());
    distinct->computeFactorizedSchema();
    leftPlan.setLastOperator(distinct);
    // PK lookup of the seed node keyed by the unwound values.
    appendFlattens(leftPlan.getSchema()->getGroupsPosInScope(), leftPlan);
    auto seedProps = getProperties(*seedNode);
    seedProps.erase(std::remove_if(seedProps.begin(), seedProps.end(),
                        [](const std::shared_ptr<Expression>& expression) {
                            return expression->constCast<PropertyExpression>().isInternalID();
                        }),
        seedProps.end());
    const auto dependentExprs = getDependentExprs(freshElem, *leftPlan.getSchema());
    DASSERT(!dependentExprs.empty());
    const auto outputGroupPos = leftPlan.getSchema()->getGroupPos(*dependentExprs[0]);
    for ([[maybe_unused]] auto& dependentExpr : dependentExprs) {
        DASSERT(leftPlan.getSchema()->getGroupPos(*dependentExpr) == outputGroupPos);
    }
    auto lookup =
        std::make_shared<LogicalQueryPrimaryKeyLookup>(seedTableID, seedNode->getInternalID(),
            seedProps, freshElem, outputGroupPos, leftPlan.getLastOperator());
    lookup->computeFactorizedSchema();
    lookup->setCardinality(leftPlan.getCardinality());
    leftPlan.setLastOperator(std::move(lookup));
    // Walk the chain.
    auto curObj = seedNode;
    for (auto& step : steps) {
        auto dir = chainExtendDirection(*step.rel, *curObj);
        appendExtend(curObj, step.nbr, step.rel, dir, getProperties(*step.rel), leftPlan);
        curObj = step.nbr;
    }
    // Anchor meet: PK scan + one extend, the meeting node's properties, then the meet
    // join. Skipped in anchor-free full-walk mode.
    if (meetRel != nullptr) {
        LogicalPlan anchorPlan;
        appendScanNodeTable(anchorNode->getInternalID(), anchorNode->getTableIDs(),
            getProperties(*anchorNode), anchorPlan, anchorNode.get());
        appendFilter(predicates[anchorPredIdx], anchorPlan);
        auto meetDir = chainExtendDirection(*meetRel, *anchorNode);
        appendExtend(anchorNode, meetNode, meetRel, meetDir, getProperties(*meetRel), anchorPlan);
        LogicalPlan propsPlan;
        appendScanNodeTable(meetNode->getInternalID(), meetNode->getTableIDs(),
            getProperties(*meetNode), propsPlan, meetNode.get());
        appendHashJoin(expression_vector{meetNode->getInternalID()}, JoinType::INNER, anchorPlan,
            propsPlan, anchorPlan);
        appendHashJoin(expression_vector{meetNode->getInternalID()}, JoinType::INNER, leftPlan,
            anchorPlan, leftPlan);
    }
    // Attach any still-missing leg-node properties (extends bind IDs only; the seed
    // lookup and anchor scan carry their own).
    for (auto i = 0u; i < queryGraph->getNumQueryNodes(); ++i) {
        auto node = queryGraph->getQueryNode(i);
        expression_vector missing;
        for (auto& prop : getProperties(*node)) {
            if (!leftPlan.getSchema()->isExpressionInScope(*prop)) {
                missing.push_back(prop);
            }
        }
        if (missing.empty()) {
            continue;
        }
        LogicalPlan propsPlan;
        appendScanNodeTable(node->getInternalID(), node->getTableIDs(), missing, propsPlan,
            node.get());
        appendHashJoin(expression_vector{node->getInternalID()}, JoinType::INNER, leftPlan,
            propsPlan, leftPlan);
    }
    // Residual predicates (none may touch leg variables by construction).
    for (auto i = 0u; i < predicates.size(); ++i) {
        if (static_cast<int>(i) == convertIdx || static_cast<int>(i) == anchorPredIdx) {
            continue;
        }
        appendFilter(predicates[i], leftPlan);
    }
    return true;
}

// ---- Scalar-seeded chain for correlated PK equalities (Q6) ----
// Shape: MATCH <leg> WHERE n.Prop = <outer scalar> with n.Prop the node's single-table
// primary key (e.g. Q6's t.ID = knownTagId, bound outside). The equality is
// outer-dependent, so planRegularMatch would pull it above the join while the DP
// enumerator plans the leg standalone from full scans. Instead the leg is decorrelated:
// an ACCUMULATE + EXPRESSIONS_SCAN pair (the standard correlated-execution plumbing,
// linked by CorrelatedSubqueryUnnestSolver) feeds DISTINCT key values into a PK lookup
// of the seed node, tree-walk extends cover every rel (outer-bound neighbors first, so
// the chain correlates early), and the chain hash-joins back onto the outer plan on the
// correlated node IDs plus the key (airtight under multi-valued keys). Residuals
// evaluable in the chain apply before the join, the rest after; unfetched leg-node
// properties attach afterwards like the property-seeded chain above.
// Fires only for a single-graph, non-recursive leg with exactly one convertible PK
// equality (outer-evaluable key with an outer dependent; pure constants stay on the
// regular path), at least one outer-correlated node ID to join back on, and full
// acyclic rel coverage. Anything else returns false and the caller falls back untouched.
// Compared to the closed PR #1062 this needs no deep copy of the outer prefix (no
// operator audit list), no fresh variable copies (the build starts from the key alone,
// so the leg's own nodes are unbound and never collide with the outer scope), and no
// SIP flag on the main join (ACCUMULATE-probe joins skip SIP in HashJoinSIPOptimizer).
bool Planner::tryPlanScalarSeededChain(const QueryGraphCollection& queryGraphCollection,
    const expression_vector& predicates, LogicalPlan& leftPlan) {
    if (!clientContext->getClientConfig()->enablePlanOptimizer) {
        return false;
    }
    if (leftPlan.isEmpty() || leftPlan.hasUpdate() ||
        queryGraphCollection.getNumQueryGraphs() != 1) {
        return false;
    }
    auto queryGraph = queryGraphCollection.getQueryGraph(0);
    if (queryGraph->getNumQueryRels() == 0) {
        return false;
    }
    for (auto i = 0u; i < queryGraph->getNumQueryRels(); ++i) {
        if (queryGraph->getQueryRel(i)->getRelType() != QueryRelType::NON_RECURSIVE) {
            return false;
        }
    }
    auto outerScope = collectOuterScopeNames(*leftPlan.getSchema());
    auto legVars = collectGraphVarNames(queryGraphCollection);
    for (auto& pred : predicates) {
        if (isListContainsFunc(pred) || containsSubqueryOrLambda(pred)) {
            return false;
        }
    }
    // 1. The single convertible PK equality: node PK vs. an outer-evaluable key with at
    // least one outer dependent. A constant key (no dependents) never qualifies, so this
    // cannot steal legs better served by constant-driven planning.
    std::shared_ptr<NodeExpression> seedNode;
    std::shared_ptr<Expression> seedKey;
    int convertIdx = -1;
    for (auto i = 0u; i < predicates.size(); ++i) {
        auto& pred = predicates[i];
        if (pred->expressionType != ExpressionType::EQUALS) {
            continue;
        }
        auto lhs = pred->getChild(0);
        auto rhs = pred->getChild(1);
        for (auto n = 0u; n < queryGraph->getNumQueryNodes(); ++n) {
            auto node = queryGraph->getQueryNode(n);
            if (node->getTableIDs().size() != 1) {
                continue;
            }
            auto tableID = node->getTableIDs()[0];
            auto l = lhs;
            auto r = rhs;
            if (isNodePrimaryKey(*r, *node, tableID)) {
                std::swap(l, r);
            }
            if (!isNodePrimaryKey(*l, *node, tableID)) {
                continue;
            }
            if (!leftPlan.getSchema()->evaluable(*r) ||
                getDependentExprs(r, *leftPlan.getSchema()).empty()) {
                continue;
            }
            if (convertIdx >= 0) {
                return false;
            }
            convertIdx = static_cast<int>(i);
            seedNode = node;
            seedKey = r;
        }
    }
    if (convertIdx < 0) {
        return false;
    }
    if (outerScope.contains(seedNode->getUniqueName()) ||
        outerScope.contains(seedNode->getInternalID()->getUniqueName())) {
        return false;
    }
    auto seedTableID = seedNode->getTableIDs()[0];
    auto seedTable = storage::StorageManager::Get(*clientContext)
                         ->getTable(seedTableID)
                         ->ptrCast<storage::NodeTable>();
    if (seedTable->tryGetPrimaryKeyIndex() == nullptr) {
        return false;
    }
    // Every other predicate must be a (possibly deferred) filter: no hidden constructs,
    // and all dependent variables bound by the chain (leg vars) or already bound (outer).
    for (auto i = 0u; i < predicates.size(); ++i) {
        if (static_cast<int>(i) == convertIdx) {
            continue;
        }
        auto collector = DependentVarNameCollector();
        collector.visit(predicates[i]);
        for (auto& var : collector.getVarNames()) {
            if (!legVars.contains(var) && !outerScope.contains(var)) {
                return false;
            }
        }
    }
    // 2. Spanning-tree coverage from the seed. Steps toward outer-bound neighbors are
    // preferred: they correlate (filter) the chain early, while unbound fan-outs (Q6:
    // post->tag at ~3.5 per post) would otherwise multiply rows that later steps discard.
    // A rel with both ends bound is a cycle: bail. Unlike #1062 no fresh copies are
    // needed: the build below starts from the key alone, so every leg node is unbound
    // there and extending to it never collides with the outer scope.
    struct WalkStep {
        std::shared_ptr<RelExpression> rel;
        std::shared_ptr<NodeExpression> bound;
        std::shared_ptr<NodeExpression> nbr;
    };
    std::unordered_set<std::string> boundNames{seedNode->getUniqueName()};
    std::unordered_set<std::string> usedRels;
    std::vector<WalkStep> steps;
    auto numRels = queryGraph->getNumQueryRels();
    while (usedRels.size() < numRels) {
        std::optional<WalkStep> outerStep;
        std::optional<WalkStep> plainStep;
        for (auto i = 0u; i < numRels; ++i) {
            auto rel = queryGraph->getQueryRel(i);
            if (usedRels.contains(rel->getUniqueName())) {
                continue;
            }
            auto srcName = rel->getSrcNode()->getUniqueName();
            auto dstName = rel->getDstNode()->getUniqueName();
            if (srcName == dstName) {
                return false;
            }
            auto srcBound = boundNames.contains(srcName);
            auto dstBound = boundNames.contains(dstName);
            if (srcBound && dstBound) {
                return false;
            }
            std::shared_ptr<NodeExpression> boundObj;
            std::shared_ptr<NodeExpression> nbrObj;
            if (srcBound && !dstBound) {
                boundObj = rel->getSrcNode();
                nbrObj = rel->getDstNode();
            } else if (dstBound && !srcBound) {
                boundObj = rel->getDstNode();
                nbrObj = rel->getSrcNode();
            } else {
                continue;
            }
            auto isOuter = outerScope.contains(nbrObj->getInternalID()->getUniqueName());
            if (isOuter && !outerStep.has_value()) {
                outerStep = WalkStep{rel, boundObj, nbrObj};
            } else if (!isOuter && !plainStep.has_value()) {
                plainStep = WalkStep{rel, boundObj, nbrObj};
            }
        }
        const WalkStep* chosen = nullptr;
        if (outerStep.has_value()) {
            chosen = &outerStep.value();
        } else if (plainStep.has_value()) {
            chosen = &plainStep.value();
        } else {
            return false;
        }
        boundNames.insert(chosen->nbr->getUniqueName());
        steps.push_back(*chosen);
        usedRels.insert(chosen->rel->getUniqueName());
    }
    // Correlated node IDs to join back on (seed itself is never outer-bound: checked).
    std::vector<std::string> corrIDNames;
    for (auto i = 0u; i < queryGraph->getNumQueryNodes(); ++i) {
        auto node = queryGraph->getQueryNode(i);
        if (outerScope.contains(node->getInternalID()->getUniqueName())) {
            corrIDNames.push_back(node->getInternalID()->getUniqueName());
        }
    }
    if (corrIDNames.empty()) {
        return false;
    }
    auto keyOuter = findInScope(*leftPlan.getSchema(), seedKey->getUniqueName());
    if (keyOuter == nullptr) {
        return false;
    }
    // Resolve the probe-side join objects before mutating leftPlan (ACCUMULATE preserves
    // the expressions, so the shared_ptrs stay valid afterwards).
    expression_vector probeIDs;
    for (auto& name : corrIDNames) {
        auto probeObj = findInScope(*leftPlan.getSchema(), name);
        if (probeObj == nullptr) {
            return false;
        }
        probeIDs.push_back(probeObj);
    }
    // 3. Build the chain in an independent plan: DISTINCT keys -> PK lookup -> extends.
    // All fallible work happens here, before leftPlan is touched.
    cardinalityEstimator.init(*queryGraph);
    LogicalPlan build;
    appendExpressionsScan(expression_vector{keyOuter}, build);
    build.getLastOperator()->setCardinality(leftPlan.getCardinality());
    auto keyInBuild = findInScope(*build.getSchema(), keyOuter->getUniqueName());
    if (keyInBuild == nullptr) {
        return false;
    }
    appendDistinct(expression_vector{keyInBuild}, build);
    auto keyScoped = findInScope(*build.getSchema(), keyOuter->getUniqueName());
    if (keyScoped == nullptr) {
        return false;
    }
    appendFlattens(build.getSchema()->getGroupsPosInScope(), build);
    auto seedProps = getProperties(*seedNode);
    seedProps.erase(std::remove_if(seedProps.begin(), seedProps.end(),
                        [](const std::shared_ptr<Expression>& expression) {
                            return expression->constCast<PropertyExpression>().isInternalID();
                        }),
        seedProps.end());
    const auto dependentExprs = getDependentExprs(keyScoped, *build.getSchema());
    if (dependentExprs.empty()) {
        return false;
    }
    const auto outputGroupPos = build.getSchema()->getGroupPos(*dependentExprs[0]);
    auto lookup = std::make_shared<LogicalQueryPrimaryKeyLookup>(seedTableID,
        seedNode->getInternalID(), seedProps, keyScoped, outputGroupPos, build.getLastOperator());
    lookup->computeFactorizedSchema();
    lookup->setCardinality(build.getCardinality());
    build.setLastOperator(std::move(lookup));
    for (auto& step : steps) {
        auto dir = chainExtendDirection(*step.rel, *step.bound);
        appendExtend(step.bound, step.nbr, step.rel, dir, getProperties(*step.rel), build);
    }
    expression_vector deferred;
    for (auto i = 0u; i < predicates.size(); ++i) {
        if (static_cast<int>(i) == convertIdx) {
            continue;
        }
        if (build.getSchema()->evaluable(*predicates[i])) {
            appendFilter(predicates[i], build);
        } else {
            deferred.push_back(predicates[i]);
        }
    }
    auto keyBuild = findInScope(*build.getSchema(), keyOuter->getUniqueName());
    if (keyBuild == nullptr) {
        return false;
    }
    std::vector<expression_pair> joinConditions;
    for (auto i = 0u; i < corrIDNames.size(); ++i) {
        auto buildObj = findInScope(*build.getSchema(), corrIDNames[i]);
        if (buildObj == nullptr) {
            return false;
        }
        joinConditions.emplace_back(probeIDs[i], buildObj);
    }
    joinConditions.emplace_back(keyOuter, keyBuild);
    // 4. Decorrelate: ACCUMULATE the outer keys (links the EXPRESSIONS_SCAN above via
    // CorrelatedSubqueryUnnestSolver) and hash-join the chain back.
    appendAccumulate(expression_vector{keyOuter}, leftPlan);
    appendHashJoin(joinConditions, JoinType::INNER, nullptr, leftPlan, build, leftPlan);
    for (auto i = 0u; i < queryGraph->getNumQueryNodes(); ++i) {
        auto node = queryGraph->getQueryNode(i);
        expression_vector missing;
        for (auto& prop : getProperties(*node)) {
            if (!leftPlan.getSchema()->isExpressionInScope(*prop)) {
                missing.push_back(prop);
            }
        }
        if (missing.empty()) {
            continue;
        }
        LogicalPlan propsPlan;
        appendScanNodeTable(node->getInternalID(), node->getTableIDs(), missing, propsPlan,
            node.get());
        appendHashJoin(expression_vector{node->getInternalID()}, JoinType::INNER, leftPlan,
            propsPlan, leftPlan);
        // The probe carries the whole outer row, so a probe-to-build mask materializes
        // wide rows to prune a small dimension scan: never worth it. Build-to-probe
        // masks still apply.
        leftPlan.getLastOperator()->cast<LogicalHashJoin>().getSIPInfoUnsafe().position =
            SemiMaskPosition::PROHIBIT_PROBE_TO_BUILD;
    }
    for (auto& pred : deferred) {
        appendFilter(pred, leftPlan);
    }
    return true;
}

// ---- Staged distinct pre-aggregation over LEFT-join chains ----
// Shape: AGGREGATE(keys=K, aggs=[COUNT DISTINCT x_1 .. x_m]) over a left-deep chain
// of LEFT joins J_1..J_n (n>=2, each probe = previous output, build = one leg), e.g.
// LDBC SNB Q14's four OPTIONAL MATCH diamonds feeding four COUNT(DISTINCT)s. The legs
// compound multiplicatively (656k rows from a single-row outer) into one final hash
// aggregation. COUNT(DISTINCT) is invariant under row duplication and ignores NULLs,
// so each leg's distinct set can be counted as soon as its leg joins, grouping by the
// same keys K: insert AGGREGATE(keys=K, aggs=[CDs of leg i]) between J_i and J_{i+1}
// and shrink the top to AGGREGATE(keys=K, aggs=[CDs of leg n], payloads=[CDs of legs
// below]). Row counts collapse per stage (Q14: 656k -> ~300) while every join keeps
// its original keys and every expression keeps its unique name.
// Soundness conditions (anything else: untouched):
// - every aggregate is COUNT DISTINCT (duplication-invariant; anything else aborts);
// - only PROJECTION/FLATTEN (pass-through for the old agg, dropped with it) may sit
//   between the agg and the top LEFT join; the chain itself is LEFT-only, left-deep;
// - every group-key dependency is bound at the chain base (stages regroup by K);
// - every COUNT DISTINCT arg comes from exactly one leg's build scope, never the base
//   (assigns each count to the stage that first sees its values);
// - everything carried through a stage aggregation (group-key deps for regrouping K,
//   later probe-side join keys) is provably constant within a K-group: identical to
//   a key, a child of K's single LIST_CREATION pack (list equality is elementwise),
//   or node-determined (property/pattern of var v with v's internal ID or primary
//   key in the determinant set). Payload counts from earlier stages are constant per
//   group structurally (one probe row per group fans out downstream).
// Runs at appendAggregate time (planner): ancestors above do not exist yet, so no
// schema recompute above is needed; schemas along the edited spine are recomputed
// bottom-up with the standard append machinery. Accumulates feeding correlated legs
// sit below each insertion point and keep seeing full rows.

static std::unordered_set<std::string> collectOpScopeNames(planner::LogicalOperator* op) {
    std::unordered_set<std::string> names;
    if (op == nullptr || op->getSchema() == nullptr) {
        return names;
    }
    for (auto& expr : op->getSchema()->getExpressionsInScope()) {
        names.insert(expr->getUniqueName());
    }
    return names;
}

static std::unordered_set<std::string> collectOpDepNames(const std::shared_ptr<Expression>& expr,
    planner::LogicalOperator* scopeOp) {
    std::unordered_set<std::string> names;
    if (scopeOp == nullptr || scopeOp->getSchema() == nullptr) {
        return names;
    }
    for (auto& dep : getDependentExprs(expr, *scopeOp->getSchema())) {
        names.insert(dep->getUniqueName());
    }
    return names;
}

static bool isCountDistinctAgg(const std::shared_ptr<Expression>& expr,
    std::shared_ptr<Expression>& argOut) {
    if (expr->expressionType != ExpressionType::AGGREGATE_FUNCTION) {
        return false;
    }
    auto& aggFunc = expr->constCast<AggregateFunctionExpression>();
    if (aggFunc.getFunction().name != function::CountFunction::name || !aggFunc.isDistinct() ||
        aggFunc.getNumChildren() != 1) {
        return false;
    }
    argOut = aggFunc.getChild(0);
    return true;
}

static bool isListPackExpr(const std::shared_ptr<Expression>& expr) {
    if (expr->expressionType != ExpressionType::FUNCTION) {
        return false;
    }
    return expr->constCast<ScalarFunctionExpression>().getFunction().name ==
           function::ListCreationFunction::name;
}

// True when needExpr takes a single value per K-group (see header comment).
static bool isKDetermined(const std::shared_ptr<Expression>& needExpr,
    const std::unordered_set<std::string>& kNames, const std::unordered_set<std::string>& kPackDeps,
    planner::LogicalOperator* scopeOp) {
    auto uname = needExpr->getUniqueName();
    if (kNames.contains(uname) || kPackDeps.contains(uname)) {
        return true;
    }
    std::string varName;
    if (needExpr->expressionType == ExpressionType::PROPERTY) {
        varName = needExpr->constCast<PropertyExpression>().getVariableName();
    } else if (ExpressionUtil::isNodePattern(*needExpr) ||
               ExpressionUtil::isRelPattern(*needExpr)) {
        varName = uname;
    } else {
        return false;
    }
    if (scopeOp == nullptr || scopeOp->getSchema() == nullptr) {
        return false;
    }
    for (auto& expr : scopeOp->getSchema()->getExpressionsInScope()) {
        auto ename = expr->getUniqueName();
        if (ExpressionUtil::isNodePattern(*expr) || ExpressionUtil::isRelPattern(*expr)) {
            if (ename != varName) {
                continue;
            }
            std::shared_ptr<Expression> internalID;
            if (ExpressionUtil::isNodePattern(*expr)) {
                internalID = expr->constCast<NodeExpression>().getInternalID();
            } else {
                internalID = expr->constCast<RelExpression>().getInternalID();
            }
            if (kNames.contains(internalID->getUniqueName()) ||
                kPackDeps.contains(internalID->getUniqueName())) {
                return true;
            }
        } else if (expr->expressionType == ExpressionType::PROPERTY) {
            auto& prop = expr->constCast<PropertyExpression>();
            if (prop.getVariableName() == varName && prop.isPrimaryKey() &&
                (kNames.contains(ename) || kPackDeps.contains(ename))) {
                return true;
            }
        }
    }
    return false;
}

bool Planner::tryPreAggregateDistinctLeftChain(const expression_vector& keys,
    const expression_vector& aggregates, LogicalPlan& plan) {
    if (plan.isEmpty() || keys.empty()) {
        return false;
    }
    struct CountDistinct {
        std::shared_ptr<Expression> agg;
        std::shared_ptr<Expression> arg;
    };
    std::vector<CountDistinct> cds;
    for (auto& agg : aggregates) {
        CountDistinct cd;
        if (!isCountDistinctAgg(agg, cd.arg)) {
            return false;
        }
        cd.agg = agg;
        cds.push_back(std::move(cd));
    }
    auto cur = plan.getLastOperator();
    while (cur->getOperatorType() == LogicalOperatorType::PROJECTION ||
           cur->getOperatorType() == LogicalOperatorType::FLATTEN) {
        if (cur->getNumChildren() != 1) {
            return false;
        }
        cur = cur->getChild(0);
    }
    struct ChainLeg {
        std::shared_ptr<LogicalOperator> joinOp;
        std::shared_ptr<LogicalOperator> build;
    };
    std::vector<ChainLeg> legs; // legs[0] = deepest join J_1 after reverse
    while (cur->getOperatorType() == LogicalOperatorType::HASH_JOIN &&
           cur->constCast<LogicalHashJoin>().getJoinType() == JoinType::LEFT) {
        if (cur->getNumChildren() != 2) {
            return false;
        }
        legs.push_back({cur, cur->getChild(1)});
        cur = cur->getChild(0);
    }
    if (legs.size() < 2) {
        return false;
    }
    std::reverse(legs.begin(), legs.end());
    const auto n = legs.size();
    auto baseOp = cur;
    auto baseNames = collectOpScopeNames(baseOp.get());
    std::unordered_set<std::string> kNames;
    std::unordered_set<std::string> kDepNames;
    for (auto& key : keys) {
        kNames.insert(key->getUniqueName());
        for (auto& dep : collectOpDepNames(key, baseOp.get())) {
            kDepNames.insert(dep);
        }
    }
    if (kDepNames.empty()) {
        return false;
    }
    for (auto& dep : kDepNames) {
        if (!baseNames.contains(dep)) {
            return false;
        }
    }
    std::unordered_set<std::string> kPackDeps;
    if (keys.size() == 1 && isListPackExpr(keys[0])) {
        for (auto& child : keys[0]->getChildren()) {
            kPackDeps.insert(child->getUniqueName());
        }
    }
    std::vector<std::vector<std::shared_ptr<Expression>>> legAggs(n);
    std::vector<std::unordered_set<std::string>> buildNames(n);
    for (auto i = 0u; i < n; ++i) {
        buildNames[i] = collectOpScopeNames(legs[i].build.get());
    }
    for (auto& cd : cds) {
        auto argName = cd.arg->getUniqueName();
        if (baseNames.contains(argName)) {
            return false;
        }
        int owner = -1;
        for (auto i = 0u; i < n; ++i) {
            if (buildNames[i].contains(argName)) {
                if (owner >= 0) {
                    return false;
                }
                owner = static_cast<int>(i);
            }
        }
        if (owner < 0) {
            return false;
        }
        legAggs[owner].push_back(cd.agg);
    }
    // NEED[i]: carried by stage i (group-key deps for regrouping K, later probe-side
    // join keys, earlier counts). Probe-side key deps of J_{i+1}.. (pair.first).
    std::unordered_set<std::string> cdNames;
    for (auto& cd : cds) {
        cdNames.insert(cd.agg->getUniqueName());
    }
    std::vector<expression_vector> needExprs(n);
    for (auto i = 0u; i + 1 < n; ++i) {
        std::unordered_set<std::string> names = kDepNames;
        for (auto j = i + 1; j < n; ++j) {
            auto probeScopeOp = legs[j].joinOp->getChild(0).get();
            for (auto& cond : legs[j].joinOp->constCast<LogicalHashJoin>().getJoinConditions()) {
                for (auto& dep : collectOpDepNames(cond.first, probeScopeOp)) {
                    names.insert(dep);
                }
            }
        }
        for (auto j = 0u; j <= i; ++j) {
            for (auto& aggObj : legAggs[j]) {
                names.insert(aggObj->getUniqueName());
            }
        }
        auto stageScopeOp = legs[i].joinOp.get();
        if (stageScopeOp->getSchema() == nullptr) {
            return false;
        }
        auto stageNames = collectOpScopeNames(stageScopeOp);
        expression_vector exprs;
        for (auto& name : names) {
            if (kNames.contains(name) || cdNames.contains(name)) {
                continue;
            }
            if (!stageNames.contains(name)) {
                return false;
            }
            auto found = findInScope(*stageScopeOp->getSchema(), name);
            if (found == nullptr || !isKDetermined(found, kNames, kPackDeps, stageScopeOp)) {
                return false;
            }
            exprs.push_back(found);
        }
        for (auto j = 0u; j <= i; ++j) {
            for (auto& aggObj : legAggs[j]) {
                exprs.push_back(aggObj);
            }
        }
        needExprs[i] = std::move(exprs);
    }
    auto appendStageAgg = [&](std::shared_ptr<LogicalOperator> childTop,
                              const expression_vector& stageAggs,
                              const expression_vector& payloads) {
        auto stageAgg = std::make_shared<LogicalAggregate>(keys, stageAggs, childTop);
        stageAgg->setDependentKeys(payloads);
        for (auto groupPos : stageAgg->getGroupsPosToFlatten()) {
            auto child = stageAgg->getChild(0);
            auto group = child->getSchema()->getGroup(groupPos);
            if (group->isFlat()) {
                continue;
            }
            auto flatten = std::make_shared<LogicalFlatten>(groupPos, child,
                cardinalityEstimator.estimateFlatten(*child, groupPos));
            flatten->computeFactorizedSchema();
            stageAgg->setChild(0, flatten);
        }
        stageAgg->computeFactorizedSchema();
        // Output rows = distinct K values. K is carried by the probe side and LEFT joins
        // preserve probe rows, so distinct K values cannot exceed probe-side rows — a bound
        // the generic group-count estimator cannot see (it only sees the compounded
        // join output). Lock it so CardinalityUpdater preserves it.
        const auto childTopProbeCard = childTop->getNumChildren() > 0 ?
                                           childTop->getChild(0)->getCardinality() :
                                           stageAgg->getChild(0)->getCardinality();
        stageAgg->setCardinality(std::min(cardinalityEstimator.estimateAggregate(*stageAgg),
            std::max<cardinality_t>(childTopProbeCard, 1)));
        stageAgg->setCardinalityLocked(true);
        return stageAgg;
    };
    // Materialize K above the chain base so every stage input carries the group key
    // as a bound column (aggregates group by scope columns; K is otherwise only
    // evaluable from its deps, which the hash-aggregate mapper cannot resolve).
    // Joins pass probe columns through, so one projection serves all stages.
    if (baseOp->getSchema() != nullptr) {
        expression_vector projExprs;
        for (auto& expr : baseOp->getSchema()->getExpressionsInScope()) {
            projExprs.push_back(expr);
        }
        bool needProj = false;
        for (auto& key : keys) {
            bool present = false;
            for (auto& expr : projExprs) {
                if (expr->getUniqueName() == key->getUniqueName()) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                projExprs.push_back(key);
                needProj = true;
            }
        }
        if (needProj) {
            auto keyProj = std::make_shared<LogicalProjection>(projExprs, baseOp);
            keyProj->computeFactorizedSchema();
            keyProj->setCardinality(baseOp->getCardinality());
            legs[0].joinOp->setChild(0, keyProj);
            for (auto i = 0u; i < n; ++i) {
                legs[i].joinOp->computeFactorizedSchema();
            }
        }
    }
    for (auto i = 0u; i + 1 < n; ++i) {
        auto stageAgg = appendStageAgg(legs[i].joinOp, legAggs[i], needExprs[i]);
        legs[i + 1].joinOp->setChild(0, stageAgg);
    }
    for (auto i = 0u; i < n; ++i) {
        legs[i].joinOp->computeFactorizedSchema();
    }
    expression_vector topPayloads;
    for (auto i = 0u; i + 1 < n; ++i) {
        for (auto& aggObj : legAggs[i]) {
            topPayloads.push_back(aggObj);
        }
    }
    auto topAgg = appendStageAgg(legs[n - 1].joinOp, legAggs[n - 1], topPayloads);
    plan.setLastOperator(topAgg);
    return true;
}

void Planner::planOptionalMatch(const QueryGraphCollection& queryGraphCollection,
    const expression_vector& predicates, std::shared_ptr<Expression> mark, LogicalPlan& leftPlan,
    std::shared_ptr<BoundJoinHintNode> hint) {
    expression_vector legPredicates = predicates;
    // MERGE existence checks (mark != nullptr) and explicit join hints keep legacy
    // behavior end to end (see below): the collect-membership rewrite above is skipped
    // for them too.
    bool legacyPath = mark != nullptr || hint != nullptr;
    if (!leftPlan.isEmpty() && !legacyPath) {
        // Unnest COLLECT-then-IN into (group key, element) rows before anything else;
        // on success legPredicates holds equalities and leftPlan carries element rows.
        tryUnnestCollectMembership(queryGraphCollection, legPredicates, leftPlan);
    }
    expression_vector correlatedExprs;
    if (!leftPlan.isEmpty()) {
        correlatedExprs =
            getCorrelatedExprs(queryGraphCollection, legPredicates, leftPlan.getSchema());
    }
    auto info = QueryGraphPlanningInfo();
    info.hint = hint;
    if (leftPlan.isEmpty()) {
        // Optional match is the first clause, e.g. OPTIONAL MATCH <pattern> RETURN *
        info.predicates = legPredicates;
        auto plan = planQueryGraphCollection(queryGraphCollection, info);
        leftPlan.setLastOperator(plan.getLastOperator());
        appendOptionalAccumulate(mark, leftPlan);
        return;
    }
    if (correlatedExprs.empty()) {
        // Plan uncorrelated subquery (think of this as a CTE)
        info.predicates = legPredicates;
        auto rightPlan = planQueryGraphCollection(queryGraphCollection, info);
        if (leftPlan.hasUpdate()) {
            appendAccOptionalCrossProduct(mark, leftPlan, rightPlan, leftPlan);
        } else {
            appendOptionalCrossProduct(mark, leftPlan, rightPlan, leftPlan);
        }
        return;
    }
    // Plan correlated subquery
    // Legacy predicate analysis (no constant-equality rerouting): rerouting into the
    // subplan breaks MERGE's existence-mark semantics (node-pattern values such as
    // a.ID = 100 must stay on the outer join), and hints pin a join order that only
    // exists under legacy analysis.
    info.corrExprsCard = leftPlan.getCardinality();
    auto analyzer =
        SubqueryPredicatePullUpAnalyzer(*leftPlan.getSchema(), queryGraphCollection, !legacyPath);
    bool canUnnest = analyzer.analyze(legPredicates);
    // Unnesting re-plans the subquery standalone and joins afterward. That wins when the
    // inner plan is selective on its own, i.e. every correlated node carries a constant
    // filter inside the clause (e.g. LDBC SNB complex-14's legs filter a.ID and b.ID).
    // comes only from the outer bindings, the standalone inner plan scans everything
    // (e.g. LDBC SNB complex-7's like-branches would read all persons' likes) and correlated
    // with outer-driven semi masks wins, so keep the correlated plan in that case.
    bool innerSelective = true;
    if (canUnnest && !leftPlan.isEmpty()) {
        std::unordered_set<std::string> constFilteredVars;
        for (auto& pred : legPredicates) {
            if (!isRoutableConstantEquality(pred, queryGraphCollection)) {
                continue;
            }
            auto collector = DependentVarNameCollector();
            collector.visit(pred);
            // Note: getVarNames() returns by value, so bind it once. Calling it twice for
            // begin()/end() would make the insert range span two different temporaries
            // (undefined behavior: the loop can spin forever or walk freed memory).
            auto predVarNames = collector.getVarNames();
            constFilteredVars.insert(predVarNames.begin(), predVarNames.end());
        }
        for (auto& node : queryGraphCollection.getQueryNodes()) {
            if (leftPlan.getSchema()->isExpressionInScope(*node->getInternalID()) &&
                !constFilteredVars.contains(node->getUniqueName())) {
                innerSelective = false;
                break;
            }
        }
    }
    // Recursive patterns (and the path variables they feed) do not unnest correctly:
    // their path construction relies on correlated execution. Keep them correlated.
    bool hasRecursiveRel = false;
    for (auto& rel : queryGraphCollection.getQueryRels()) {
        if (common::QueryRelTypeUtils::isRecursive(rel->getRelType())) {
            hasRecursiveRel = true;
            break;
        }
    }
    // Legacy paths keep the pre-change branch decision as well: the selective-unnest gate
    // below must not redirect a plan the legacy analysis chose to unnest (or vice versa).
    CorrelatedOptionalLegDecision decision;
    decision.legacyPath = legacyPath;
    decision.canUnnest = canUnnest;
    decision.innerSelective = innerSelective;
    decision.hasRecursiveRel = hasRecursiveRel;
    if (canUnnest) {
        for (auto& joinCondition : analyzer.getJoinConditions()) {
            if (joinCondition.first->getUniqueName() != joinCondition.second->getUniqueName()) {
                decision.hasCrossVariableJoin = true;
                break;
            }
        }
    }
    // Pure eligibility check (no plan mutation); the builder below revalidates.
    std::shared_ptr<Expression> pkLookupKey;
    expression_vector pkLookupResiduals;
    decision.pkLookupEligible = findCorrelatedPrimaryKeyLookupKey(queryGraphCollection,
        legPredicates, correlatedExprs, *leftPlan.getSchema(), pkLookupKey, pkLookupResiduals);
    std::vector<expression_pair> joinConditions;
    LogicalPlan rightPlan;
    const auto strategy = decision.decide();
    if (strategy == CorrelatedOptionalLegDecision::Strategy::PRIMARY_KEY_LOOKUP &&
        tryPlanCorrelatedPrimaryKeyLookup(queryGraphCollection, legPredicates, correlatedExprs,
            *leftPlan.getSchema(), info.corrExprsCard, rightPlan)) {
        // A single node matched by primary key against outer expressions needs no
        // table scan: probe the PK index per outer binding instead (Left Join
        // semantics preserved by PK uniqueness).
        info.subqueryType = SubqueryPlanningType::CORRELATED;
        info.corrExprs = correlatedExprs;
        info.predicates = legPredicates;
        for (auto& expr : correlatedExprs) {
            joinConditions.emplace_back(expr, expr);
        }
        appendAccumulate(correlatedExprs, leftPlan);
    } else if (strategy == CorrelatedOptionalLegDecision::Strategy::UNNEST_LEFT_JOIN) {
        // Unnest as left join
        info.subqueryType = SubqueryPlanningType::UNNEST_CORRELATED;
        info.corrExprs = analyzer.getCorrelatedInternalIDs();
        info.predicates = analyzer.getNonCorrelatedPredicates();
        rightPlan = planQueryGraphCollectionInNewContext(queryGraphCollection, info);
        joinConditions = analyzer.getJoinConditions();
    } else {
        // Correlated: expression scan + distinct & inner join. Also the safe fallback
        // if a PK-eligible leg ever fails to build (eligibility and builder agree by
        // construction; see findCorrelatedPrimaryKeyLookupKey).
        info.subqueryType = SubqueryPlanningType::CORRELATED;
        info.corrExprs = correlatedExprs;
        info.predicates = legPredicates;
        for (auto& expr : correlatedExprs) {
            joinConditions.emplace_back(expr, expr);
        }
        rightPlan = planQueryGraphCollectionInNewContext(queryGraphCollection, info);
        appendAccumulate(correlatedExprs, leftPlan);
    }
    if (leftPlan.hasUpdate()) {
        appendAccHashJoin(joinConditions, JoinType::LEFT, mark, leftPlan, rightPlan, leftPlan);
    } else {
        appendHashJoin(joinConditions, JoinType::LEFT, mark, leftPlan, rightPlan, leftPlan);
    }
}

void Planner::planRegularMatch(const QueryGraphCollection& queryGraphCollection,
    const expression_vector& predicates, LogicalPlan& leftPlan,
    std::shared_ptr<BoundJoinHintNode> hint) {
    // Correlated subqueries and OPTIONAL MATCH use dedicated join planning paths. Extending this
    // lookup to them requires a rewrite that preserves their Mark/Left Join semantics.
    if (tryPlanQueryPrimaryKeyLookup(queryGraphCollection, predicates, leftPlan)) {
        return;
    }
    // Property collect-membership with a PK-seeded chain (e.g. Q12's tag.ID IN tags):
    // consumes the whole leg when applicable.
    if (tryPlanPropertySeededChain(queryGraphCollection, predicates, leftPlan)) {
        return;
    }
    // Scalar-seeded chain for correlated PK equalities (e.g. Q6's t.ID = knownTagId).
    if (tryPlanScalarSeededChain(queryGraphCollection, predicates, leftPlan)) {
        return;
    }
    expression_vector predicatesToPushDown, predicatesToPullUp;
    // E.g. MATCH (a) WITH COUNT(*) AS s MATCH (b) WHERE b.age > s
    // "b.age > s" should be pulled up after both MATCH clauses are joined.
    for (auto& predicate : predicates) {
        if (getDependentExprs(predicate, *leftPlan.getSchema()).empty()) {
            predicatesToPushDown.push_back(predicate);
        } else {
            predicatesToPullUp.push_back(predicate);
        }
    }
    auto correlatedExprs =
        getCorrelatedExprs(queryGraphCollection, predicatesToPushDown, leftPlan.getSchema());
    auto joinNodeIDs =
        ExpressionUtil::getExpressionsWithDataType(correlatedExprs, LogicalTypeID::INTERNAL_ID);
    auto info = QueryGraphPlanningInfo();
    info.predicates = predicatesToPushDown;
    info.hint = hint;
    if (joinNodeIDs.empty()) {
        info.subqueryType = SubqueryPlanningType::NONE;
        auto rightPlan = planQueryGraphCollectionInNewContext(queryGraphCollection, info);
        if (leftPlan.hasUpdate()) {
            appendCrossProduct(rightPlan, leftPlan, leftPlan);
        } else {
            appendCrossProduct(leftPlan, rightPlan, leftPlan);
        }
    } else {
        // TODO(Xiyang): there is a question regarding if we want to plan as a correlated subquery
        // Multi-part query is actually CTE and CTE can be considered as a subquery but does not
        // scan from outer.
        info.subqueryType = SubqueryPlanningType::UNNEST_CORRELATED;
        info.corrExprs = joinNodeIDs;
        info.corrExprsCard = leftPlan.getCardinality();
        auto rightPlan = planQueryGraphCollectionInNewContext(queryGraphCollection, info);
        if (leftPlan.hasUpdate()) {
            appendHashJoin(joinNodeIDs, JoinType::INNER, rightPlan, leftPlan, leftPlan);
        } else {
            appendHashJoin(joinNodeIDs, JoinType::INNER, leftPlan, rightPlan, leftPlan);
        }
    }
    for (auto& predicate : predicatesToPullUp) {
        appendFilter(predicate, leftPlan);
    }
}

void Planner::planSubquery(const std::shared_ptr<Expression>& expression, LogicalPlan& outerPlan) {
    DASSERT(expression->expressionType == ExpressionType::SUBQUERY);
    auto subquery = expression->ptrCast<SubqueryExpression>();
    auto correlatedExprs = getDependentExprs(expression, *outerPlan.getSchema());
    auto predicates = subquery->getPredicatesSplitOnAnd();
    LogicalPlan innerPlan;
    auto info = QueryGraphPlanningInfo();
    info.hint = subquery->getHint();
    if (correlatedExprs.empty()) {
        // Plan uncorrelated subquery
        info.subqueryType = SubqueryPlanningType::NONE;
        info.predicates = predicates;
        innerPlan =
            planQueryGraphCollectionInNewContext(*subquery->getQueryGraphCollection(), info);
        expression_vector emptyHashKeys;
        auto projectExprs = expression_vector{subquery->getProjectionExpr()};
        switch (subquery->getSubqueryType()) {
        case common::SubqueryType::EXISTS: {
            auto aggregates = expression_vector{subquery->getCountStarExpr()};
            appendAggregate(emptyHashKeys, aggregates, innerPlan);
            appendProjection(projectExprs, innerPlan);
        } break;
        case common::SubqueryType::COUNT: {
            appendAggregate(emptyHashKeys, projectExprs, innerPlan);
        } break;
        default:
            UNREACHABLE_CODE;
        }
        appendCrossProduct(outerPlan, innerPlan, outerPlan);
        return;
    }
    // Plan correlated subquery
    info.corrExprsCard = outerPlan.getCardinality();
    auto analyzer = SubqueryPredicatePullUpAnalyzer(*outerPlan.getSchema(),
        *subquery->getQueryGraphCollection());
    std::vector<expression_pair> joinConditions;
    if (analyzer.analyze(predicates)) {
        // Unnest as inner join
        info.subqueryType = SubqueryPlanningType::UNNEST_CORRELATED;
        info.corrExprs = analyzer.getCorrelatedInternalIDs();
        info.predicates = analyzer.getNonCorrelatedPredicates();
        innerPlan =
            planQueryGraphCollectionInNewContext(*subquery->getQueryGraphCollection(), info);
        joinConditions = analyzer.getJoinConditions();
    } else {
        // Unnest as expression scan + distinct & inner join
        info.subqueryType = SubqueryPlanningType::CORRELATED;
        info.corrExprs = correlatedExprs;
        info.predicates = predicates;
        for (auto& expr : correlatedExprs) {
            joinConditions.emplace_back(expr, expr);
        }
        innerPlan =
            planQueryGraphCollectionInNewContext(*subquery->getQueryGraphCollection(), info);
        appendAccumulate(correlatedExprs, outerPlan);
    }
    switch (subquery->getSubqueryType()) {
    case common::SubqueryType::EXISTS: {
        appendMarkJoin(joinConditions, expression, outerPlan, innerPlan, outerPlan);
    } break;
    case common::SubqueryType::COUNT: {
        expression_vector hashKeys;
        for (auto& joinCondition : joinConditions) {
            hashKeys.push_back(joinCondition.second);
        }
        appendAggregate(hashKeys, expression_vector{subquery->getProjectionExpr()}, innerPlan);
        appendHashJoin(joinConditions, common::JoinType::COUNT, nullptr, outerPlan, innerPlan,
            outerPlan);
    } break;
    default:
        UNREACHABLE_CODE;
    }
}

void Planner::planSubqueryIfNecessary(std::shared_ptr<Expression> expression, LogicalPlan& plan) {
    auto collector = SubqueryExprCollector();
    collector.visit(expression);
    if (collector.hasSubquery()) {
        for (auto& expr : collector.getSubqueryExprs()) {
            if (plan.getSchema()->isExpressionInScope(*expr)) {
                continue;
            }
            planSubquery(expr, plan);
        }
    }
}

} // namespace planner
} // namespace lbug

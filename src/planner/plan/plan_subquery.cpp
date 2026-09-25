#include <algorithm>
#include <unordered_set>

#include "binder/expression/expression_util.h"
#include "binder/expression/property_expression.h"
#include "binder/expression/subquery_expression.h"
#include "binder/expression_visitor.h"
#include "planner/operator/factorization/flatten_resolver.h"
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
bool Planner::tryPlanCorrelatedPrimaryKeyLookup(const QueryGraphCollection& queryGraphCollection,
    const expression_vector& predicates, const expression_vector& corrExprs,
    const Schema& outerSchema, cardinality_t corrExprsCard, LogicalPlan& rightPlan) {
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
        // The key must be computable from the correlated bindings alone: every outer
        // expression it depends on has to be part of the expressions scan below.
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

void Planner::planOptionalMatch(const QueryGraphCollection& queryGraphCollection,
    const expression_vector& predicates, std::shared_ptr<Expression> mark, LogicalPlan& leftPlan,
    std::shared_ptr<BoundJoinHintNode> hint) {
    expression_vector correlatedExprs;
    if (!leftPlan.isEmpty()) {
        correlatedExprs =
            getCorrelatedExprs(queryGraphCollection, predicates, leftPlan.getSchema());
    }
    auto info = QueryGraphPlanningInfo();
    info.hint = hint;
    if (leftPlan.isEmpty()) {
        // Optional match is the first clause, e.g. OPTIONAL MATCH <pattern> RETURN *
        info.predicates = predicates;
        auto plan = planQueryGraphCollection(queryGraphCollection, info);
        leftPlan.setLastOperator(plan.getLastOperator());
        appendOptionalAccumulate(mark, leftPlan);
        return;
    }
    if (correlatedExprs.empty()) {
        // Plan uncorrelated subquery (think of this as a CTE)
        info.predicates = predicates;
        auto rightPlan = planQueryGraphCollection(queryGraphCollection, info);
        if (leftPlan.hasUpdate()) {
            appendAccOptionalCrossProduct(mark, leftPlan, rightPlan, leftPlan);
        } else {
            appendOptionalCrossProduct(mark, leftPlan, rightPlan, leftPlan);
        }
        return;
    }
    // Plan correlated subquery
    // MERGE existence checks (mark != nullptr) and explicit join hints keep legacy predicate
    // analysis: rerouting constant equalities into the subplan breaks MERGE's existence-mark
    // semantics (node-pattern values such as a.ID = 100 must stay on the outer join), and
    // hints pin a join order that only exists under legacy analysis.
    bool legacyPath = mark != nullptr || hint != nullptr;
    info.corrExprsCard = leftPlan.getCardinality();
    auto analyzer =
        SubqueryPredicatePullUpAnalyzer(*leftPlan.getSchema(), queryGraphCollection, !legacyPath);
    bool canUnnest = analyzer.analyze(predicates);
    // Unnesting re-plans the subquery standalone and joins afterward. That wins when the
    // inner plan is selective on its own, i.e. every correlated node carries a constant
    // filter inside the clause (e.g. Q14's legs filter a.ID and b.ID). When selectivity
    // comes only from the outer bindings, the standalone inner plan scans everything
    // (e.g. Q7's like-branches would read all persons' likes) and correlated execution
    // with outer-driven semi masks wins, so keep the correlated plan in that case.
    bool innerSelective = true;
    if (canUnnest && !leftPlan.isEmpty()) {
        std::unordered_set<std::string> constFilteredVars;
        for (auto& pred : predicates) {
            if (!isRoutableConstantEquality(pred, queryGraphCollection)) {
                continue;
            }
            auto collector = DependentVarNameCollector();
            collector.visit(pred);
            constFilteredVars.insert(collector.getVarNames().begin(),
                collector.getVarNames().end());
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
    std::vector<expression_pair> joinConditions;
    LogicalPlan rightPlan;
    if (canUnnest && (legacyPath || (innerSelective && !hasRecursiveRel))) {
        // Unnest as left join
        info.subqueryType = SubqueryPlanningType::UNNEST_CORRELATED;
        info.corrExprs = analyzer.getCorrelatedInternalIDs();
        info.predicates = analyzer.getNonCorrelatedPredicates();
        rightPlan = planQueryGraphCollectionInNewContext(queryGraphCollection, info);
        joinConditions = analyzer.getJoinConditions();
    } else {
        // Unnest as expression scan + distinct & inner join
        info.subqueryType = SubqueryPlanningType::CORRELATED;
        info.corrExprs = correlatedExprs;
        info.predicates = predicates;
        for (auto& expr : correlatedExprs) {
            joinConditions.emplace_back(expr, expr);
        }
        // A single node matched by primary key against outer expressions needs no
        // table scan: probe the PK index per outer binding instead (Left Join
        // semantics preserved by PK uniqueness). Skipped on legacy paths (MERGE
        // existence checks, join hints), which require the legacy correlated shape below.
        if (legacyPath ||
            !tryPlanCorrelatedPrimaryKeyLookup(queryGraphCollection, predicates, correlatedExprs,
                *leftPlan.getSchema(), info.corrExprsCard, rightPlan)) {
            rightPlan = planQueryGraphCollectionInNewContext(queryGraphCollection, info);
        }
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

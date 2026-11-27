#include "planner/operator/operator_properties.h"

#include "binder/expression/property_expression.h"
#include "common/types/types.h"
#include "planner/operator/logical_accumulate.h"
#include "planner/operator/logical_aggregate.h"
#include "planner/operator/logical_cross_product.h"
#include "planner/operator/logical_hash_join.h"
#include "planner/operator/logical_order_by.h"
#include "planner/operator/logical_table_function_call.h"
#include "planner/operator/scan/logical_scan_node_table.h"

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace planner {

namespace {

// Returns whether a scan node table has no row-dropping predicates or index restrictions.
bool isUnfilteredFullNodeScan(const LogicalScanNodeTable& scan) {
    switch (scan.getScanType()) {
    case LogicalScanNodeTableType::SCAN:
        break;
    case LogicalScanNodeTableType::PRIMARY_KEY_SCAN:
    case LogicalScanNodeTableType::SECONDARY_INDEX_SCAN:
        return false;
    }
    for (const auto& predSet : scan.getPropertyPredicates()) {
        if (!predSet.isEmpty()) {
            return false;
        }
    }
    return true;
}

} // namespace

bool isUnfilteredUniqueNodeBuild(const LogicalOperator& buildRoot, const Expression& joinKey) {
    const LogicalOperator* current = &buildRoot;
    while (current != nullptr) {
        if (current->getOperatorType() == LogicalOperatorType::SCAN_NODE_TABLE) {
            const auto& scan = current->constCast<LogicalScanNodeTable>();
            return isUnfilteredFullNodeScan(scan) && *scan.getNodeID() == joinKey;
        }
        const auto traits = getStaticOperatorTraits(current->getOperatorType());
        if (traits.dynamicKind != DynamicSemanticsKind::NONE ||
            traits.child0Cardinality != RowCardinalityEffect::EXACT_ONE ||
            current->getNumChildren() != 1) {
            return false;
        }
        current = current->getChild(0).get();
    }
    return false;
}

bool keyCanBeNull(const LogicalOperator& op, const Expression& key) {
    const auto sem = analyzeOperator(op);
    switch (sem.nullSupply) {
    case NullSupplyBehavior::NEVER_NULLS:
        break;
    case NullSupplyBehavior::MAY_NULL_BUILD_ONLY: {
        // Build-only columns from an unmarked LEFT join or OPTIONAL cross product can be NULL.
        if (op.getNumChildren() >= 2 && !op.getChild(0)->getSchema()->isExpressionInScope(key) &&
            op.getChild(1)->getSchema()->isExpressionInScope(key)) {
            return true;
        }
        break;
    }
    case NullSupplyBehavior::MAY_NULL_ANY: {
        // For an unmarked OPTIONAL accumulate, all columns from its child can be NULL.
        if (op.getOperatorType() == LogicalOperatorType::ACCUMULATE && op.getNumChildren() == 1 &&
            op.getChild(0)->getSchema()->isExpressionInScope(key)) {
            return true;
        }
        break;
    }
    }
    for (auto i = 0u; i < op.getNumChildren(); ++i) {
        if (keyCanBeNull(*op.getChild(i), key)) {
            return true;
        }
    }
    return false;
}

bool isGuaranteedOneToOneInnerJoin(const LogicalHashJoin& join) {
    if (join.getJoinType() != JoinType::INNER) {
        return false;
    }
    const auto& conditions = join.getJoinConditions();
    if (conditions.size() != 1) {
        return false;
    }
    const auto& [probeKey, buildKey] = conditions[0];
    if (probeKey->expressionType != ExpressionType::PROPERTY ||
        buildKey->expressionType != ExpressionType::PROPERTY) {
        return false;
    }
    const auto& probeProp = probeKey->constCast<PropertyExpression>();
    const auto& buildProp = buildKey->constCast<PropertyExpression>();
    if (!probeProp.isInternalID() || !buildProp.isInternalID() ||
        probeProp.getUniqueName() != buildProp.getUniqueName()) {
        return false;
    }
    if (!isUnfilteredUniqueNodeBuild(*join.getChild(1), *buildKey)) {
        return false;
    }
    return !keyCanBeNull(*join.getChild(0), *probeKey);
}

OperatorSemantics analyzeOperator(const LogicalOperator& op) {
    const auto type = op.getOperatorType();
    const auto staticTraits = getStaticOperatorTraits(type);
    auto sem = OperatorSemantics::fromStatic(staticTraits);
    // Exhaustive over DynamicSemanticsKind (no default: label).
    switch (staticTraits.dynamicKind) {
    case DynamicSemanticsKind::NONE:
        return sem;
    case DynamicSemanticsKind::HASH_JOIN: {
        const auto& join = op.constCast<LogicalHashJoin>();
        const auto joinType = join.getJoinType();
        sem.child0Cardinality = joinProbeCardinalityEffect(joinType);
        sem.nullSupply = (joinNullsBuildOnlyColumns(joinType) && !join.hasMark()) ?
                             NullSupplyBehavior::MAY_NULL_BUILD_ONLY :
                             NullSupplyBehavior::NEVER_NULLS;
        switch (joinType) {
        case JoinType::LEFT:
        case JoinType::MARK:
        case JoinType::COUNT:
            sem.limitAction = LimitPushDownAction::PUSH_TO_PROBE_CHILD;
            break;
        case JoinType::INNER:
            if (isGuaranteedOneToOneInnerJoin(join)) {
                sem.child0Cardinality = RowCardinalityEffect::EXACT_ONE;
                sem.limitAction = LimitPushDownAction::PUSH_TO_PROBE_CHILD;
            } else {
                sem.limitAction = LimitPushDownAction::STOP;
            }
            break;
        }
        return sem;
    }
    case DynamicSemanticsKind::CROSS_PRODUCT: {
        const auto& cp = op.constCast<LogicalCrossProduct>();
        const bool optional = isOptionalAccumulate(cp.getAccumulateType());
        sem.child0Cardinality =
            optional ? RowCardinalityEffect::AT_LEAST_ONE : RowCardinalityEffect::GLOBAL;
        sem.nullSupply = (optional && !cp.hasMark()) ? NullSupplyBehavior::MAY_NULL_BUILD_ONLY :
                                                       NullSupplyBehavior::NEVER_NULLS;
        sem.limitAction = LimitPushDownAction::PUSH_TO_ALL_CHILDREN;
        return sem;
    }
    case DynamicSemanticsKind::ACCUMULATE: {
        const auto& acc = op.constCast<LogicalAccumulate>();
        const bool optional = isOptionalAccumulate(acc.getAccumulateType());
        sem.child0Cardinality =
            optional ? RowCardinalityEffect::GLOBAL : RowCardinalityEffect::EXACT_ONE;
        sem.nullSupply = (optional && !acc.hasMark()) ? NullSupplyBehavior::MAY_NULL_ANY :
                                                        NullSupplyBehavior::NEVER_NULLS;
        sem.limitAction = LimitPushDownAction::CAP_AND_PASS_THROUGH_CHILD_0;
        sem.filterAction = (!optional && !acc.hasMark()) ?
                               FilterPushDownAction::PASS_THROUGH_IF_EVALUABLE :
                               FilterPushDownAction::STOP;
        return sem;
    }
    case DynamicSemanticsKind::ORDER_BY: {
        const auto& orderBy = op.constCast<LogicalOrderBy>();
        const bool hasBound = orderBy.hasLimitNum() || orderBy.hasSkipNum();
        if (hasBound) {
            sem.limitAction = LimitPushDownAction::STOP;
            sem.orderByAction = OrderByPushDownAction::STOP;
            sem.filterAction = FilterPushDownAction::STOP;
        } else {
            sem.limitAction = LimitPushDownAction::ABSORB_NATIVE;
            sem.orderByAction = OrderByPushDownAction::COMBINE_ORDER_BY;
            sem.filterAction = FilterPushDownAction::PASS_THROUGH_IF_EVALUABLE;
        }
        return sem;
    }
    case DynamicSemanticsKind::AGGREGATE: {
        const auto& agg = op.constCast<LogicalAggregate>();
        sem.filterAction =
            agg.hasKeys() ? FilterPushDownAction::SPLIT_ON_GROUP_KEYS : FilterPushDownAction::STOP;
        return sem;
    }
    case DynamicSemanticsKind::TABLE_FUNCTION_CALL: {
        const auto& tfc = op.constCast<LogicalTableFunctionCall>();
        if (tfc.getTableFunc().supportsPushDownFunc()) {
            sem.limitAction = LimitPushDownAction::ABSORB_NATIVE;
            sem.orderByAction = OrderByPushDownAction::ABSORB_NATIVE;
            sem.filterAction = FilterPushDownAction::ABSORB_LEAF;
        } else {
            sem.limitAction = LimitPushDownAction::STOP;
            sem.orderByAction = OrderByPushDownAction::STOP;
            sem.filterAction = FilterPushDownAction::STOP;
        }
        return sem;
    }
    case DynamicSemanticsKind::SCAN_NODE_TABLE: {
        const auto& scan = op.constCast<LogicalScanNodeTable>();
        switch (scan.getScanType()) {
        case LogicalScanNodeTableType::SCAN:
        case LogicalScanNodeTableType::PRIMARY_KEY_SCAN:
        case LogicalScanNodeTableType::SECONDARY_INDEX_SCAN:
            sem.filterAction = FilterPushDownAction::ABSORB_LEAF;
            break;
        }
        return sem;
    }
    }
    return sem;
}

bool subtreeHasSideEffects(const LogicalOperator& op) {
    if (getStaticOperatorTraits(op.getOperatorType()).sideEffect != SideEffectClass::PURE) {
        return true;
    }
    for (auto i = 0u; i < op.getNumChildren(); ++i) {
        if (subtreeHasSideEffects(*op.getChild(i))) {
            return true;
        }
    }
    return false;
}

bool subtreeCanSupplyNulls(const LogicalOperator& op) {
    // Match the conservative semantics of plan_subquery.cpp: MARK joins also count as
    // potentially null-affecting for collect-membership unnesting, and any non-REGULAR
    // ACCUMULATE counts as null-supplying even when marked.
    if (op.getOperatorType() == LogicalOperatorType::HASH_JOIN) {
        const auto joinType = op.constCast<LogicalHashJoin>().getJoinType();
        switch (joinType) {
        case JoinType::LEFT:
        case JoinType::MARK:
            return true;
        case JoinType::INNER:
        case JoinType::COUNT:
            break;
        }
    } else if (op.getOperatorType() == LogicalOperatorType::ACCUMULATE) {
        if (isOptionalAccumulate(op.constCast<LogicalAccumulate>().getAccumulateType())) {
            return true;
        }
    } else if (analyzeOperator(op).nullSupply != NullSupplyBehavior::NEVER_NULLS) {
        return true;
    }
    for (auto i = 0u; i < op.getNumChildren(); ++i) {
        if (subtreeCanSupplyNulls(*op.getChild(i))) {
            return true;
        }
    }
    return false;
}

bool isUniqueKeyPreservingSubtree(const LogicalOperator& op, const Expression* requiredNodeID) {
    const LogicalOperator* cur = &op;
    while (cur != nullptr) {
        const auto type = cur->getOperatorType();
        if (type == LogicalOperatorType::SCAN_NODE_TABLE) {
            if (requiredNodeID != nullptr) {
                return *cur->constCast<LogicalScanNodeTable>().getNodeID() == *requiredNodeID;
            }
            return true;
        }
        if (type == LogicalOperatorType::INDEX_LOOK_UP && requiredNodeID == nullptr) {
            return true;
        }
        const auto sem = analyzeOperator(*cur);
        // MULTIPLICITY_REDUCER is AT_LEAST_ONE in general (expands multiplicity > 1),
        // but on a linear chain rooted at a unique node scan (which always has multiplicity 1),
        // it cannot introduce duplicates.
        const bool allowsThrough =
            sem.neverDuplicatesChild0Rows() || type == LogicalOperatorType::MULTIPLICITY_REDUCER;
        if (!allowsThrough || cur->getNumChildren() != 1) {
            return false;
        }
        cur = cur->getChild(0).get();
    }
    return false;
}

} // namespace planner
} // namespace lbug

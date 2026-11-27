#pragma once

#include <array>
#include <cstdint>

#include "common/api.h"
#include "common/enums/accumulate_type.h"
#include "common/enums/join_type.h"
#include "planner/operator/logical_operator.h"

namespace lbug {
namespace binder {
class Expression;
}
namespace planner {

class LogicalHashJoin;

// Observable side effects of an operator. Exhaustively classified for every LogicalOperatorType.
enum class SideEffectClass : uint8_t {
    // Pure read-only relational operator.
    PURE = 0,
    // Mutates persistent graph data (INSERT, DELETE, SET_PROPERTY, MERGE, COPY_FROM).
    DATA_WRITE = 1,
    // DDL, transaction control, I/O, or session state mutation.
    META_OR_IO = 2,
};

// Per-input-row output multiplicity with respect to child 0 (or the probe child for joins).
// Forms a 2-bit lattice over (canDropRows, canDuplicateRows) plus GLOBAL for set/multi-child ops.
enum class RowCardinalityEffect : uint8_t {
    // [1, 1]: never drops an input row and never duplicates it.
    EXACT_ONE = 0,
    // [1, inf): never drops an input row; may amplify/duplicate it.
    AT_LEAST_ONE = 1,
    // [0, 1]: may drop input rows; never amplifies/duplicates them.
    AT_MOST_ONE = 2,
    // [0, inf): may both drop and duplicate input rows.
    ZERO_OR_MORE = 3,
    // Set-level, multi-child combining, or 0-child leaf/DDL operator.
    GLOBAL = 4,
};

// How an operator affects tuple ordering from child 0.
enum class OrderPreservation : uint8_t {
    // Preserves the input order of child 0.
    PRESERVES_ORDER = 0,
    // Establishes a new tuple order (ORDER_BY).
    PRODUCES_ORDER = 1,
    // Reorders tuples or merges multi-threaded state non-deterministically.
    BREAKS_ORDER = 2,
    // 0-child source, sink, or non-query statement.
    NONE = 3,
};

// Whether an operator can introduce NULL bindings for variables in scope.
enum class NullSupplyBehavior : uint8_t {
    // Only passes through, filters, or combines already-bound non-null rows.
    NEVER_NULLS = 0,
    // May NULL out build-only (child 1) expressions (unmarked LEFT join / OPTIONAL cross product).
    MAY_NULL_BUILD_ONLY = 1,
    // May introduce NULL bindings on any output expression (unmarked OPTIONAL accumulate, UNWIND,
    // EXPRESSIONS_SCAN, TABLE_FUNCTION_CALL, UNION_ALL, or conservative fallback).
    MAY_NULL_ANY = 2,
};

// Execution pipeline boundary behavior on child 0.
enum class PipelineBehavior : uint8_t {
    // Pulls child 0 lazily on demand; an upstream LIMIT stops pulling early.
    STREAMING = 0,
    // Materializes its entire child before producing the first output row.
    PIPELINE_BREAKER = 1,
    // 0-child source or terminal sink/DDL.
    LEAF_OR_SINK = 2,
};

// Exhaustive action for LimitPushDownOptimizer.
enum class LimitPushDownAction : uint8_t {
    // Hard barrier: cannot push row demand below this operator.
    STOP = 0,
    // Transparent streaming pass-through (1:1 or >=1 per input row) to child 0.
    PASS_THROUGH_CHILD_0 = 1,
    // Materializing 1:1 / optional sink (ACCUMULATE): install a physical cap directly below it
    // (when the child subtree is side-effect-free) and continue pushing into child 0.
    CAP_AND_PASS_THROUGH_CHILD_0 = 2,
    // LogicalLimit: tighten the active skip/limit demand and continue into child 0.
    COMBINE_LIMIT = 3,
    // Operator natively supports early termination (TABLE_FUNCTION_CALL with pushdown, DISTINCT,
    // RECURSIVE_EXTEND, or un-limited ORDER_BY absorbing a TOP_K cap).
    ABSORB_NATIVE = 4,
    // Join whose probe child (child 0) produces >= 1 output row per probe row (LEFT, MARK, COUNT,
    // or provably 1:1 non-null INNER join): push upper-bound cap to child 0.
    PUSH_TO_PROBE_CHILD = 5,
    // Multi-child operator where capping every child at skip + limit preserves exact semantics
    // when the outer SKIP/LIMIT stays in place (UNION_ALL, CROSS_PRODUCT for cap >= 1).
    PUSH_TO_ALL_CHILDREN = 6,
};

// Exhaustive action for OrderByPushDownOptimizer.
enum class OrderByPushDownAction : uint8_t {
    // Hard barrier (includes LIMIT, ACCUMULATE, AGGREGATE, DISTINCT, joins, EXTEND, writes, ...).
    STOP = 0,
    // Strictly order-preserving, non-limiting single-child pass-through.
    PASS_THROUGH_CHILD_0 = 1,
    // Inner ORDER_BY: combine or keep local depending on whether sort keys push down.
    COMBINE_ORDER_BY = 2,
    // Native sort pushdown target (TABLE_FUNCTION_CALL with supportsPushDownFunc()).
    ABSORB_NATIVE = 3,
};

// Exhaustive action for predicate / filter pushdown.
enum class FilterPushDownAction : uint8_t {
    // Hard barrier (LIMIT, limited ORDER_BY, OPTIONAL ACCUMULATE, writes, DDL, ...).
    STOP = 0,
    // LogicalFilter: absorb non-volatile predicates (folding literal TRUE/FALSE/NULL).
    ABSORB_FILTER = 1,
    // Unary operator through which evaluable predicates can pass unchanged.
    PASS_THROUGH_IF_EVALUABLE = 2,
    // Binary join / cross product: route predicates according to JoinType / AccumulateType.
    SPLIT_BY_JOIN_TYPE = 3,
    // UNION_ALL: multi-branch union boundary.
    PUSH_TO_ALL_CHILDREN = 4,
    // AGGREGATE with grouping keys / DISTINCT: push predicates composed solely of keys.
    SPLIT_ON_GROUP_KEYS = 5,
    // EXTEND / PACKED_EXTEND: attach zone-map predicates and push child-only predicates.
    ABSORB_ZONE_MAP_AND_PASS = 6,
    // Leaf scan / table function that consumes column or index predicates.
    ABSORB_LEAF = 7,
};

// Identifies which operators require instance inspection beyond their LogicalOperatorType.
// Exhaustively switched in analyzeOperator() with NO default: label.
enum class DynamicSemanticsKind : uint8_t {
    NONE = 0,
    HASH_JOIN = 1,
    CROSS_PRODUCT = 2,
    ACCUMULATE = 3,
    ORDER_BY = 4,
    AGGREGATE = 5,
    TABLE_FUNCTION_CALL = 6,
    SCAN_NODE_TABLE = 7,
};

// Static, type-level algebraic traits for a LogicalOperatorType.
// Every LogicalOperatorType enumerator is classified in getStaticOperatorTraits() with NO default:
// label, so -Wswitch (and MSVC /we4062) turns an unclassified new operator into a build error.
struct StaticOperatorTraits {
    SideEffectClass sideEffect;
    DynamicSemanticsKind dynamicKind;
    RowCardinalityEffect child0Cardinality;
    OrderPreservation orderPreservation;
    NullSupplyBehavior nullSupply;
    PipelineBehavior pipelineBehavior;
    LimitPushDownAction limitAction;
    OrderByPushDownAction orderByAction;
    FilterPushDownAction filterAction;
};

// Resolved, instance-aware algebraic semantics for a concrete LogicalOperator node.
struct OperatorSemantics {
    SideEffectClass sideEffect;
    RowCardinalityEffect child0Cardinality;
    OrderPreservation orderPreservation;
    NullSupplyBehavior nullSupply;
    PipelineBehavior pipelineBehavior;
    LimitPushDownAction limitAction;
    OrderByPushDownAction orderByAction;
    FilterPushDownAction filterAction;

    static constexpr OperatorSemantics fromStatic(const StaticOperatorTraits& traits) {
        return OperatorSemantics{
            traits.sideEffect,
            traits.child0Cardinality,
            traits.orderPreservation,
            traits.nullSupply,
            traits.pipelineBehavior,
            traits.limitAction,
            traits.orderByAction,
            traits.filterAction,
        };
    }

    constexpr bool isPure() const { return sideEffect == SideEffectClass::PURE; }
    constexpr bool neverDropsChild0Rows() const {
        return child0Cardinality == RowCardinalityEffect::EXACT_ONE ||
               child0Cardinality == RowCardinalityEffect::AT_LEAST_ONE;
    }
    constexpr bool neverDuplicatesChild0Rows() const {
        return child0Cardinality == RowCardinalityEffect::EXACT_ONE ||
               child0Cardinality == RowCardinalityEffect::AT_MOST_ONE;
    }
};

#pragma push_macro("DELETE")
#undef DELETE

// Exhaustive compile-time list of all LogicalOperatorType enumerators (useful for invariant tests).
inline constexpr std::array<LogicalOperatorType, 65> ALL_LOGICAL_OPERATOR_TYPES = {
    LogicalOperatorType::ACCUMULATE,
    LogicalOperatorType::AGGREGATE,
    LogicalOperatorType::ALTER,
    LogicalOperatorType::ANALYZE,
    LogicalOperatorType::ATTACH_DATABASE,
    LogicalOperatorType::COPY_FROM,
    LogicalOperatorType::COPY_TO,
    LogicalOperatorType::COUNT_ANTI_EDGE_CHAIN,
    LogicalOperatorType::COUNT_EXTEND_CHAIN,
    LogicalOperatorType::COUNT_REL_TABLE,
    LogicalOperatorType::CREATE_CONSTRAINT,
    LogicalOperatorType::CREATE_GRAPH,
    LogicalOperatorType::CREATE_INDEX,
    LogicalOperatorType::CREATE_MACRO,
    LogicalOperatorType::CREATE_SEQUENCE,
    LogicalOperatorType::CREATE_TABLE,
    LogicalOperatorType::CREATE_TYPE,
    LogicalOperatorType::CROSS_PRODUCT,
    LogicalOperatorType::DELETE,
    LogicalOperatorType::DETACH_DATABASE,
    LogicalOperatorType::DISTINCT,
    LogicalOperatorType::DROP,
    LogicalOperatorType::DROP_CONSTRAINT,
    LogicalOperatorType::DUMMY_SCAN,
    LogicalOperatorType::DUMMY_SINK,
    LogicalOperatorType::EMPTY_RESULT,
    LogicalOperatorType::EXPLAIN,
    LogicalOperatorType::EXPORT_DATABASE,
    LogicalOperatorType::EXPRESSIONS_SCAN,
    LogicalOperatorType::EXTEND,
    LogicalOperatorType::EXTENSION,
    LogicalOperatorType::EXTENSION_CLAUSE,
    LogicalOperatorType::FILTER,
    LogicalOperatorType::FLATTEN,
    LogicalOperatorType::GROUPED_REACHABLE_COUNT,
    LogicalOperatorType::HASH_JOIN,
    LogicalOperatorType::IMPORT_DATABASE,
    LogicalOperatorType::INDEX_LOOK_UP,
    LogicalOperatorType::INSERT,
    LogicalOperatorType::INTERSECT,
    LogicalOperatorType::LIMIT,
    LogicalOperatorType::MERGE,
    LogicalOperatorType::MULTIPLICITY_REDUCER,
    LogicalOperatorType::NODE_LABEL_FILTER,
    LogicalOperatorType::NOOP,
    LogicalOperatorType::ORDER_BY,
    LogicalOperatorType::PACKED_EXTEND,
    LogicalOperatorType::PARTITIONER,
    LogicalOperatorType::PATH_PROPERTY_PROBE,
    LogicalOperatorType::PROJECTION,
    LogicalOperatorType::QUERY_PRIMARY_KEY_LOOKUP,
    LogicalOperatorType::REACHABLE_COUNT,
    LogicalOperatorType::RECURSIVE_EXTEND,
    LogicalOperatorType::REL_DEGREE_TABLE,
    LogicalOperatorType::SCAN_NODE_TABLE,
    LogicalOperatorType::SEMI_MASKER,
    LogicalOperatorType::SET_PROPERTY,
    LogicalOperatorType::STANDALONE_CALL,
    LogicalOperatorType::TABLE_FUNCTION_CALL,
    LogicalOperatorType::TRANSACTION,
    LogicalOperatorType::UNION_ALL,
    LogicalOperatorType::UNWIND,
    LogicalOperatorType::UNWIND_DEDUPLICATE,
    LogicalOperatorType::USE_DATABASE,
    LogicalOperatorType::USE_GRAPH,
};

namespace detail {

constexpr StaticOperatorTraits makeReadLeafTraits(NullSupplyBehavior nullSupply,
    LimitPushDownAction limitAction = LimitPushDownAction::STOP,
    OrderByPushDownAction orderByAction = OrderByPushDownAction::STOP,
    FilterPushDownAction filterAction = FilterPushDownAction::STOP,
    DynamicSemanticsKind dynamicKind = DynamicSemanticsKind::NONE) {
    return StaticOperatorTraits{
        SideEffectClass::PURE,
        dynamicKind,
        RowCardinalityEffect::GLOBAL,
        OrderPreservation::NONE,
        nullSupply,
        PipelineBehavior::LEAF_OR_SINK,
        limitAction,
        orderByAction,
        filterAction,
    };
}

constexpr StaticOperatorTraits makeSideEffectTraits(SideEffectClass sideEffect) {
    return StaticOperatorTraits{
        sideEffect,
        DynamicSemanticsKind::NONE,
        RowCardinalityEffect::GLOBAL,
        OrderPreservation::NONE,
        NullSupplyBehavior::MAY_NULL_ANY,
        PipelineBehavior::LEAF_OR_SINK,
        LimitPushDownAction::STOP,
        OrderByPushDownAction::STOP,
        FilterPushDownAction::STOP,
    };
}

} // namespace detail

// Authoritative exhaustive trait table for every LogicalOperatorType.
// Intentionally has NO `default:` label: -Wswitch turns any unclassified new operator into a
// compile error (see #935).
constexpr StaticOperatorTraits getStaticOperatorTraits(LogicalOperatorType type) {
    switch (type) {
    // ---- Unary transparent / pass-through operators ----
    case LogicalOperatorType::PROJECTION:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE, RowCardinalityEffect::EXACT_ONE,
            OrderPreservation::PRESERVES_ORDER, NullSupplyBehavior::NEVER_NULLS,
            PipelineBehavior::STREAMING, LimitPushDownAction::PASS_THROUGH_CHILD_0,
            OrderByPushDownAction::PASS_THROUGH_CHILD_0,
            FilterPushDownAction::PASS_THROUGH_IF_EVALUABLE};
    case LogicalOperatorType::SEMI_MASKER:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE, RowCardinalityEffect::EXACT_ONE,
            OrderPreservation::PRESERVES_ORDER, NullSupplyBehavior::NEVER_NULLS,
            PipelineBehavior::STREAMING, LimitPushDownAction::PASS_THROUGH_CHILD_0,
            OrderByPushDownAction::PASS_THROUGH_CHILD_0,
            FilterPushDownAction::PASS_THROUGH_IF_EVALUABLE};
    case LogicalOperatorType::FLATTEN:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE, RowCardinalityEffect::EXACT_ONE,
            OrderPreservation::PRESERVES_ORDER, NullSupplyBehavior::NEVER_NULLS,
            PipelineBehavior::STREAMING, LimitPushDownAction::PASS_THROUGH_CHILD_0,
            OrderByPushDownAction::PASS_THROUGH_CHILD_0,
            FilterPushDownAction::PASS_THROUGH_IF_EVALUABLE};
    case LogicalOperatorType::EXPLAIN:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE, RowCardinalityEffect::EXACT_ONE,
            OrderPreservation::PRESERVES_ORDER, NullSupplyBehavior::MAY_NULL_ANY,
            PipelineBehavior::STREAMING, LimitPushDownAction::PASS_THROUGH_CHILD_0,
            OrderByPushDownAction::PASS_THROUGH_CHILD_0,
            FilterPushDownAction::PASS_THROUGH_IF_EVALUABLE};
    case LogicalOperatorType::NOOP:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE, RowCardinalityEffect::EXACT_ONE,
            OrderPreservation::PRESERVES_ORDER, NullSupplyBehavior::MAY_NULL_ANY,
            PipelineBehavior::STREAMING, LimitPushDownAction::PASS_THROUGH_CHILD_0,
            OrderByPushDownAction::PASS_THROUGH_CHILD_0,
            FilterPushDownAction::PASS_THROUGH_IF_EVALUABLE};
    case LogicalOperatorType::MULTIPLICITY_REDUCER:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE,
            RowCardinalityEffect::AT_LEAST_ONE, OrderPreservation::PRESERVES_ORDER,
            NullSupplyBehavior::NEVER_NULLS, PipelineBehavior::STREAMING,
            LimitPushDownAction::PASS_THROUGH_CHILD_0, OrderByPushDownAction::PASS_THROUGH_CHILD_0,
            FilterPushDownAction::PASS_THROUGH_IF_EVALUABLE};
    case LogicalOperatorType::PATH_PROPERTY_PROBE:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE, RowCardinalityEffect::EXACT_ONE,
            OrderPreservation::BREAKS_ORDER, NullSupplyBehavior::NEVER_NULLS,
            PipelineBehavior::STREAMING, LimitPushDownAction::PASS_THROUGH_CHILD_0,
            OrderByPushDownAction::STOP, FilterPushDownAction::STOP};

    // ---- Row-filtering / row-limiting operators ----
    case LogicalOperatorType::FILTER:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE,
            RowCardinalityEffect::AT_MOST_ONE, OrderPreservation::PRESERVES_ORDER,
            NullSupplyBehavior::NEVER_NULLS, PipelineBehavior::STREAMING, LimitPushDownAction::STOP,
            OrderByPushDownAction::PASS_THROUGH_CHILD_0, FilterPushDownAction::ABSORB_FILTER};
    case LogicalOperatorType::NODE_LABEL_FILTER:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE,
            RowCardinalityEffect::AT_MOST_ONE, OrderPreservation::PRESERVES_ORDER,
            NullSupplyBehavior::NEVER_NULLS, PipelineBehavior::STREAMING, LimitPushDownAction::STOP,
            OrderByPushDownAction::PASS_THROUGH_CHILD_0,
            FilterPushDownAction::PASS_THROUGH_IF_EVALUABLE};
    case LogicalOperatorType::LIMIT:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE,
            RowCardinalityEffect::AT_MOST_ONE, OrderPreservation::PRESERVES_ORDER,
            NullSupplyBehavior::NEVER_NULLS, PipelineBehavior::STREAMING,
            LimitPushDownAction::COMBINE_LIMIT, OrderByPushDownAction::STOP,
            FilterPushDownAction::STOP};
    case LogicalOperatorType::INDEX_LOOK_UP:
    case LogicalOperatorType::QUERY_PRIMARY_KEY_LOOKUP:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE,
            RowCardinalityEffect::AT_MOST_ONE, OrderPreservation::PRESERVES_ORDER,
            NullSupplyBehavior::NEVER_NULLS, PipelineBehavior::STREAMING, LimitPushDownAction::STOP,
            OrderByPushDownAction::STOP, FilterPushDownAction::STOP};

    // ---- Blocking / global / deduplicating / sorting operators ----
    case LogicalOperatorType::DISTINCT:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE,
            RowCardinalityEffect::AT_MOST_ONE, OrderPreservation::BREAKS_ORDER,
            NullSupplyBehavior::NEVER_NULLS, PipelineBehavior::PIPELINE_BREAKER,
            LimitPushDownAction::ABSORB_NATIVE, OrderByPushDownAction::STOP,
            FilterPushDownAction::SPLIT_ON_GROUP_KEYS};
    case LogicalOperatorType::UNWIND_DEDUPLICATE:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE,
            RowCardinalityEffect::AT_MOST_ONE, OrderPreservation::BREAKS_ORDER,
            NullSupplyBehavior::MAY_NULL_ANY, PipelineBehavior::PIPELINE_BREAKER,
            LimitPushDownAction::STOP, OrderByPushDownAction::STOP, FilterPushDownAction::STOP};
    case LogicalOperatorType::ORDER_BY:
        return {SideEffectClass::PURE, DynamicSemanticsKind::ORDER_BY, RowCardinalityEffect::GLOBAL,
            OrderPreservation::PRODUCES_ORDER, NullSupplyBehavior::NEVER_NULLS,
            PipelineBehavior::PIPELINE_BREAKER, LimitPushDownAction::ABSORB_NATIVE,
            OrderByPushDownAction::COMBINE_ORDER_BY,
            FilterPushDownAction::PASS_THROUGH_IF_EVALUABLE};
    case LogicalOperatorType::AGGREGATE:
        return {SideEffectClass::PURE, DynamicSemanticsKind::AGGREGATE,
            RowCardinalityEffect::GLOBAL, OrderPreservation::BREAKS_ORDER,
            NullSupplyBehavior::NEVER_NULLS, PipelineBehavior::PIPELINE_BREAKER,
            LimitPushDownAction::STOP, OrderByPushDownAction::STOP, FilterPushDownAction::STOP};
    case LogicalOperatorType::ACCUMULATE:
        // Static nullSupply is conservatively MAY_NULL_ANY for type-only callers;
        // analyzeOperator() refines REGULAR vs OPTIONAL_.
        return {SideEffectClass::PURE, DynamicSemanticsKind::ACCUMULATE,
            RowCardinalityEffect::EXACT_ONE, OrderPreservation::BREAKS_ORDER,
            NullSupplyBehavior::MAY_NULL_ANY, PipelineBehavior::PIPELINE_BREAKER,
            LimitPushDownAction::CAP_AND_PASS_THROUGH_CHILD_0, OrderByPushDownAction::STOP,
            FilterPushDownAction::STOP};

    // ---- Expanding / joining / combining operators ----
    case LogicalOperatorType::EXTEND:
    case LogicalOperatorType::PACKED_EXTEND:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE,
            RowCardinalityEffect::ZERO_OR_MORE, OrderPreservation::BREAKS_ORDER,
            NullSupplyBehavior::NEVER_NULLS, PipelineBehavior::STREAMING, LimitPushDownAction::STOP,
            OrderByPushDownAction::STOP, FilterPushDownAction::ABSORB_ZONE_MAP_AND_PASS};
    case LogicalOperatorType::RECURSIVE_EXTEND:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE,
            RowCardinalityEffect::ZERO_OR_MORE, OrderPreservation::BREAKS_ORDER,
            NullSupplyBehavior::NEVER_NULLS, PipelineBehavior::STREAMING,
            LimitPushDownAction::ABSORB_NATIVE, OrderByPushDownAction::STOP,
            FilterPushDownAction::STOP};
    case LogicalOperatorType::INTERSECT:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE,
            RowCardinalityEffect::ZERO_OR_MORE, OrderPreservation::BREAKS_ORDER,
            NullSupplyBehavior::NEVER_NULLS, PipelineBehavior::STREAMING, LimitPushDownAction::STOP,
            OrderByPushDownAction::STOP, FilterPushDownAction::STOP};
    case LogicalOperatorType::UNWIND:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE,
            RowCardinalityEffect::ZERO_OR_MORE, OrderPreservation::BREAKS_ORDER,
            NullSupplyBehavior::MAY_NULL_ANY, PipelineBehavior::STREAMING,
            LimitPushDownAction::STOP, OrderByPushDownAction::STOP, FilterPushDownAction::STOP};
    case LogicalOperatorType::HASH_JOIN:
        return {SideEffectClass::PURE, DynamicSemanticsKind::HASH_JOIN,
            RowCardinalityEffect::ZERO_OR_MORE, OrderPreservation::BREAKS_ORDER,
            NullSupplyBehavior::MAY_NULL_ANY, PipelineBehavior::STREAMING,
            LimitPushDownAction::STOP, OrderByPushDownAction::STOP,
            FilterPushDownAction::SPLIT_BY_JOIN_TYPE};
    case LogicalOperatorType::CROSS_PRODUCT:
        return {SideEffectClass::PURE, DynamicSemanticsKind::CROSS_PRODUCT,
            RowCardinalityEffect::GLOBAL, OrderPreservation::BREAKS_ORDER,
            NullSupplyBehavior::NEVER_NULLS, PipelineBehavior::STREAMING,
            LimitPushDownAction::PUSH_TO_ALL_CHILDREN, OrderByPushDownAction::STOP,
            FilterPushDownAction::SPLIT_BY_JOIN_TYPE};
    case LogicalOperatorType::UNION_ALL:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE, RowCardinalityEffect::GLOBAL,
            OrderPreservation::BREAKS_ORDER, NullSupplyBehavior::MAY_NULL_ANY,
            PipelineBehavior::PIPELINE_BREAKER, LimitPushDownAction::PUSH_TO_ALL_CHILDREN,
            OrderByPushDownAction::STOP, FilterPushDownAction::PUSH_TO_ALL_CHILDREN};

    // ---- Leaf scans and specialized read operators ----
    case LogicalOperatorType::SCAN_NODE_TABLE:
        return detail::makeReadLeafTraits(NullSupplyBehavior::NEVER_NULLS,
            LimitPushDownAction::STOP, OrderByPushDownAction::STOP,
            FilterPushDownAction::ABSORB_LEAF, DynamicSemanticsKind::SCAN_NODE_TABLE);
    case LogicalOperatorType::TABLE_FUNCTION_CALL:
        return detail::makeReadLeafTraits(NullSupplyBehavior::MAY_NULL_ANY,
            LimitPushDownAction::STOP, OrderByPushDownAction::STOP, FilterPushDownAction::STOP,
            DynamicSemanticsKind::TABLE_FUNCTION_CALL);
    case LogicalOperatorType::EXPRESSIONS_SCAN:
    case LogicalOperatorType::DUMMY_SINK:
        return detail::makeReadLeafTraits(NullSupplyBehavior::MAY_NULL_ANY);
    case LogicalOperatorType::DUMMY_SCAN:
    case LogicalOperatorType::EMPTY_RESULT:
    case LogicalOperatorType::COUNT_REL_TABLE:
    case LogicalOperatorType::COUNT_EXTEND_CHAIN:
    case LogicalOperatorType::COUNT_ANTI_EDGE_CHAIN:
    case LogicalOperatorType::REACHABLE_COUNT:
    case LogicalOperatorType::GROUPED_REACHABLE_COUNT:
    case LogicalOperatorType::REL_DEGREE_TABLE:
        return detail::makeReadLeafTraits(NullSupplyBehavior::NEVER_NULLS);
    case LogicalOperatorType::PARTITIONER:
        return {SideEffectClass::PURE, DynamicSemanticsKind::NONE, RowCardinalityEffect::GLOBAL,
            OrderPreservation::BREAKS_ORDER, NullSupplyBehavior::MAY_NULL_ANY,
            PipelineBehavior::PIPELINE_BREAKER, LimitPushDownAction::STOP,
            OrderByPushDownAction::STOP, FilterPushDownAction::STOP};

    // ---- Data-writing DML operators ----
    case LogicalOperatorType::INSERT:
    case LogicalOperatorType::DELETE:
    case LogicalOperatorType::SET_PROPERTY:
    case LogicalOperatorType::MERGE:
    case LogicalOperatorType::COPY_FROM:
        return detail::makeSideEffectTraits(SideEffectClass::DATA_WRITE);

    // ---- DDL / I/O / session / metadata operators ----
    case LogicalOperatorType::ALTER:
    case LogicalOperatorType::ANALYZE:
    case LogicalOperatorType::ATTACH_DATABASE:
    case LogicalOperatorType::COPY_TO:
    case LogicalOperatorType::CREATE_CONSTRAINT:
    case LogicalOperatorType::CREATE_GRAPH:
    case LogicalOperatorType::CREATE_INDEX:
    case LogicalOperatorType::CREATE_MACRO:
    case LogicalOperatorType::CREATE_SEQUENCE:
    case LogicalOperatorType::CREATE_TABLE:
    case LogicalOperatorType::CREATE_TYPE:
    case LogicalOperatorType::DETACH_DATABASE:
    case LogicalOperatorType::DROP:
    case LogicalOperatorType::DROP_CONSTRAINT:
    case LogicalOperatorType::EXPORT_DATABASE:
    case LogicalOperatorType::EXTENSION:
    case LogicalOperatorType::EXTENSION_CLAUSE:
    case LogicalOperatorType::IMPORT_DATABASE:
    case LogicalOperatorType::STANDALONE_CALL:
    case LogicalOperatorType::TRANSACTION:
    case LogicalOperatorType::USE_DATABASE:
    case LogicalOperatorType::USE_GRAPH:
        return detail::makeSideEffectTraits(SideEffectClass::META_OR_IO);
    }
    // Unreachable for valid enumerators; kept outside switch so -Wswitch enforces exhaustiveness.
    return detail::makeSideEffectTraits(SideEffectClass::META_OR_IO);
}

#pragma pop_macro("DELETE")

// Exhaustive join-type helpers (no default: so adding a new JoinType fails to compile).
constexpr bool joinNullsBuildOnlyColumns(common::JoinType joinType) {
    switch (joinType) {
    case common::JoinType::INNER:
    case common::JoinType::MARK:
    case common::JoinType::COUNT:
        return false;
    case common::JoinType::LEFT:
        return true;
    }
    return true;
}

constexpr RowCardinalityEffect joinProbeCardinalityEffect(common::JoinType joinType) {
    switch (joinType) {
    case common::JoinType::MARK:
    case common::JoinType::COUNT:
        return RowCardinalityEffect::EXACT_ONE;
    case common::JoinType::LEFT:
        return RowCardinalityEffect::AT_LEAST_ONE;
    case common::JoinType::INNER:
        return RowCardinalityEffect::ZERO_OR_MORE;
    }
    return RowCardinalityEffect::ZERO_OR_MORE;
}

// Exhaustive accumulate-type helper (no default: so adding a new AccumulateType fails to compile).
constexpr bool isOptionalAccumulate(common::AccumulateType accumulateType) {
    switch (accumulateType) {
    case common::AccumulateType::REGULAR:
        return false;
    case common::AccumulateType::OPTIONAL_:
        return true;
    }
    return true;
}

// Resolves full instance-aware semantics for a concrete LogicalOperator.
LBUG_API OperatorSemantics analyzeOperator(const LogicalOperator& op);

// True if `op` or any descendant has SideEffectClass != PURE.
LBUG_API bool subtreeHasSideEffects(const LogicalOperator& op);

// True if `op` or any descendant can introduce NULL bindings (replaces plan_subquery.cpp's
// subtreeHasNullSupply).
LBUG_API bool subtreeCanSupplyNulls(const LogicalOperator& op);

// True if `key` may evaluate to NULL at the output of `op` due to an optional join, optional
// cross product, or optional accumulate (replaces RemoveUnnecessaryJoinOptimizer's canBeNull).
LBUG_API bool keyCanBeNull(const LogicalOperator& op, const binder::Expression& key);

// True when `buildRoot` is an unfiltered, full-table node scan (optionally wrapped in EXACT_ONE
// operators) producing each internal ID of `joinKey` exactly once.
LBUG_API bool isUnfilteredUniqueNodeBuild(const LogicalOperator& buildRoot,
    const binder::Expression& joinKey);

// True when `join` is an INNER join guaranteed to match every probe row with exactly 1 build row.
LBUG_API bool isGuaranteedOneToOneInnerJoin(const LogicalHashJoin& join);

// True when `op` is a linear chain of non-duplicating operators ending at a unique node scan
// (replaces JoinNodeIDUniquenessAnalyzer and RemoveUnnecessaryDistinctOptimizer's helper).
LBUG_API bool isUniqueKeyPreservingSubtree(const LogicalOperator& op,
    const binder::Expression* requiredNodeID = nullptr);

} // namespace planner
} // namespace lbug

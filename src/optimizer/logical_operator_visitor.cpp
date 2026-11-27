#include "optimizer/logical_operator_visitor.h"

using namespace lbug::planner;

namespace lbug {
namespace optimizer {

void LogicalOperatorVisitor::visitOperatorSwitch(LogicalOperator* op) {
    // Exhaustive over LogicalOperatorType (no default: label, -Wswitch enforced).
    switch (op->getOperatorType()) {
    case LogicalOperatorType::ACCUMULATE: {
        visitAccumulate(op);
    } break;
    case LogicalOperatorType::AGGREGATE: {
        visitAggregate(op);
    } break;
    case LogicalOperatorType::COPY_FROM: {
        visitCopyFrom(op);
    } break;
    case LogicalOperatorType::COPY_TO: {
        visitCopyTo(op);
    } break;
    case LogicalOperatorType::COUNT_REL_TABLE: {
        visitCountRelTable(op);
    } break;
    case LogicalOperatorType::CROSS_PRODUCT: {
        visitCrossProduct(op);
    } break;
    case LogicalOperatorType::DELETE: {
        visitDelete(op);
    } break;
    case LogicalOperatorType::DISTINCT: {
        visitDistinct(op);
    } break;
    case LogicalOperatorType::EMPTY_RESULT: {
        visitEmptyResult(op);
    } break;
    case LogicalOperatorType::EXPRESSIONS_SCAN: {
        visitExpressionsScan(op);
    } break;
    case LogicalOperatorType::EXTEND: {
        visitExtend(op);
    } break;
    case LogicalOperatorType::FILTER: {
        visitFilter(op);
    } break;
    case LogicalOperatorType::FLATTEN: {
        visitFlatten(op);
    } break;
    case LogicalOperatorType::GROUPED_REACHABLE_COUNT: {
        visitGroupedReachableCount(op);
    } break;
    case LogicalOperatorType::HASH_JOIN: {
        visitHashJoin(op);
    } break;
    case LogicalOperatorType::INDEX_LOOK_UP: {
        visitIndexLookUp(op);
    } break;
    case LogicalOperatorType::INSERT: {
        visitInsert(op);
    } break;
    case LogicalOperatorType::INTERSECT: {
        visitIntersect(op);
    } break;
    case LogicalOperatorType::LIMIT: {
        visitLimit(op);
    } break;
    case LogicalOperatorType::MERGE: {
        visitMerge(op);
    } break;
    case LogicalOperatorType::NODE_LABEL_FILTER: {
        visitNodeLabelFilter(op);
    } break;
    case LogicalOperatorType::ORDER_BY: {
        visitOrderBy(op);
    } break;
    case LogicalOperatorType::PATH_PROPERTY_PROBE: {
        visitPathPropertyProbe(op);
    } break;
    case LogicalOperatorType::PROJECTION: {
        visitProjection(op);
    } break;
    case LogicalOperatorType::QUERY_PRIMARY_KEY_LOOKUP: {
        visitQueryPrimaryKeyLookup(op);
    } break;
    case LogicalOperatorType::REACHABLE_COUNT: {
        visitReachableCount(op);
    } break;
    case LogicalOperatorType::RECURSIVE_EXTEND: {
        visitRecursiveExtend(op);
    } break;
    case LogicalOperatorType::REL_DEGREE_TABLE: {
        visitRelDegreeTable(op);
    } break;
    case LogicalOperatorType::SCAN_NODE_TABLE: {
        visitScanNodeTable(op);
    } break;
    case LogicalOperatorType::SET_PROPERTY: {
        visitSetProperty(op);
    } break;
    case LogicalOperatorType::TABLE_FUNCTION_CALL: {
        visitTableFunctionCall(op);
    } break;
    case LogicalOperatorType::UNION_ALL: {
        visitUnion(op);
    } break;
    case LogicalOperatorType::UNWIND: {
        visitUnwind(op);
    } break;
    case LogicalOperatorType::UNWIND_DEDUPLICATE: {
        visitUnwindDeduplicate(op);
    } break;
    // Operators without a dedicated virtual visit hook:
    case LogicalOperatorType::ALTER:
    case LogicalOperatorType::ANALYZE:
    case LogicalOperatorType::ATTACH_DATABASE:
    case LogicalOperatorType::COUNT_ANTI_EDGE_CHAIN:
    case LogicalOperatorType::COUNT_EXTEND_CHAIN:
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
    case LogicalOperatorType::DUMMY_SCAN:
    case LogicalOperatorType::DUMMY_SINK:
    case LogicalOperatorType::EXPLAIN:
    case LogicalOperatorType::EXPORT_DATABASE:
    case LogicalOperatorType::EXTENSION:
    case LogicalOperatorType::EXTENSION_CLAUSE:
    case LogicalOperatorType::IMPORT_DATABASE:
    case LogicalOperatorType::MULTIPLICITY_REDUCER:
    case LogicalOperatorType::NOOP:
    case LogicalOperatorType::PACKED_EXTEND:
    case LogicalOperatorType::PARTITIONER:
    case LogicalOperatorType::SEMI_MASKER:
    case LogicalOperatorType::STANDALONE_CALL:
    case LogicalOperatorType::TRANSACTION:
    case LogicalOperatorType::USE_DATABASE:
    case LogicalOperatorType::USE_GRAPH:
        return;
    }
}

std::shared_ptr<LogicalOperator> LogicalOperatorVisitor::visitOperatorReplaceSwitch(
    std::shared_ptr<LogicalOperator> op) {
    // Exhaustive over LogicalOperatorType (no default: label, -Wswitch enforced).
    switch (op->getOperatorType()) {
    case LogicalOperatorType::ACCUMULATE: {
        return visitAccumulateReplace(op);
    }
    case LogicalOperatorType::AGGREGATE: {
        return visitAggregateReplace(op);
    }
    case LogicalOperatorType::COPY_FROM: {
        return visitCopyFromReplace(op);
    }
    case LogicalOperatorType::COPY_TO: {
        return visitCopyToReplace(op);
    }
    case LogicalOperatorType::COUNT_REL_TABLE: {
        return visitCountRelTableReplace(op);
    }
    case LogicalOperatorType::CROSS_PRODUCT: {
        return visitCrossProductReplace(op);
    }
    case LogicalOperatorType::DELETE: {
        return visitDeleteReplace(op);
    }
    case LogicalOperatorType::DISTINCT: {
        return visitDistinctReplace(op);
    }
    case LogicalOperatorType::EMPTY_RESULT: {
        return visitEmptyResultReplace(op);
    }
    case LogicalOperatorType::EXPRESSIONS_SCAN: {
        return visitExpressionsScanReplace(op);
    }
    case LogicalOperatorType::EXTEND: {
        return visitExtendReplace(op);
    }
    case LogicalOperatorType::FILTER: {
        return visitFilterReplace(op);
    }
    case LogicalOperatorType::FLATTEN: {
        return visitFlattenReplace(op);
    }
    case LogicalOperatorType::GROUPED_REACHABLE_COUNT: {
        return visitGroupedReachableCountReplace(op);
    }
    case LogicalOperatorType::HASH_JOIN: {
        return visitHashJoinReplace(op);
    }
    case LogicalOperatorType::INDEX_LOOK_UP: {
        return visitIndexLookUpReplace(op);
    }
    case LogicalOperatorType::INSERT: {
        return visitInsertReplace(op);
    }
    case LogicalOperatorType::INTERSECT: {
        return visitIntersectReplace(op);
    }
    case LogicalOperatorType::LIMIT: {
        return visitLimitReplace(op);
    }
    case LogicalOperatorType::MERGE: {
        return visitMergeReplace(op);
    }
    case LogicalOperatorType::NODE_LABEL_FILTER: {
        return visitNodeLabelFilterReplace(op);
    }
    case LogicalOperatorType::ORDER_BY: {
        return visitOrderByReplace(op);
    }
    case LogicalOperatorType::PATH_PROPERTY_PROBE: {
        return visitPathPropertyProbeReplace(op);
    }
    case LogicalOperatorType::PROJECTION: {
        return visitProjectionReplace(op);
    }
    case LogicalOperatorType::QUERY_PRIMARY_KEY_LOOKUP: {
        return visitQueryPrimaryKeyLookupReplace(op);
    }
    case LogicalOperatorType::REACHABLE_COUNT: {
        return visitReachableCountReplace(op);
    }
    case LogicalOperatorType::RECURSIVE_EXTEND: {
        return visitRecursiveExtendReplace(op);
    }
    case LogicalOperatorType::REL_DEGREE_TABLE: {
        return visitRelDegreeTableReplace(op);
    }
    case LogicalOperatorType::SCAN_NODE_TABLE: {
        return visitScanNodeTableReplace(op);
    }
    case LogicalOperatorType::SET_PROPERTY: {
        return visitSetPropertyReplace(op);
    }
    case LogicalOperatorType::TABLE_FUNCTION_CALL: {
        return visitTableFunctionCallReplace(op);
    }
    case LogicalOperatorType::UNION_ALL: {
        return visitUnionReplace(op);
    }
    case LogicalOperatorType::UNWIND: {
        return visitUnwindReplace(op);
    }
    case LogicalOperatorType::UNWIND_DEDUPLICATE: {
        return visitUnwindDeduplicateReplace(op);
    }
    // Operators without a dedicated virtual replace hook:
    case LogicalOperatorType::ALTER:
    case LogicalOperatorType::ANALYZE:
    case LogicalOperatorType::ATTACH_DATABASE:
    case LogicalOperatorType::COUNT_ANTI_EDGE_CHAIN:
    case LogicalOperatorType::COUNT_EXTEND_CHAIN:
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
    case LogicalOperatorType::DUMMY_SCAN:
    case LogicalOperatorType::DUMMY_SINK:
    case LogicalOperatorType::EXPLAIN:
    case LogicalOperatorType::EXPORT_DATABASE:
    case LogicalOperatorType::EXTENSION:
    case LogicalOperatorType::EXTENSION_CLAUSE:
    case LogicalOperatorType::IMPORT_DATABASE:
    case LogicalOperatorType::MULTIPLICITY_REDUCER:
    case LogicalOperatorType::NOOP:
    case LogicalOperatorType::PACKED_EXTEND:
    case LogicalOperatorType::PARTITIONER:
    case LogicalOperatorType::SEMI_MASKER:
    case LogicalOperatorType::STANDALONE_CALL:
    case LogicalOperatorType::TRANSACTION:
    case LogicalOperatorType::USE_DATABASE:
    case LogicalOperatorType::USE_GRAPH:
        return op;
    }
    return op;
}

} // namespace optimizer
} // namespace lbug

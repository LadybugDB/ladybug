#pragma once

#include "planner/operator/operator_print_info.h"
#include "processor/result/result_set.h"

namespace lbug::common {
class Profiler;
class NumericMetric;
class TimeMetric;
} // namespace lbug::common
namespace lbug {
namespace processor {
struct ExecutionContext;

using physical_op_id = uint32_t;

// Order-preservation type for a physical operator, used by
// PhysicalPlanUtil::getOrderPreservation to walk the plan and decide which
// Arrow result-collector strategy to use.
//
// Ladybug does not expose a `preserve_insertion_order` setting to the user,
// and we assume the default that no operator makes an insertion-order
// guarantee unless it explicitly opts in by overriding operatorOrder() /
// sourceOrder() to return INSERTION_ORDER. The FIXED_ORDER overrides on
// OrderBy / TopK drive the expensive deterministic-merge collector path.
enum class OrderPreservationType : uint8_t {
    // The operator makes no guarantees on output order. Default for all
    // operators; safe to assume unless explicitly overridden. Routes to the
    // batch-index parallel collector.
    NO_ORDER,
    // The operator maintains the order of its child(ren). Reserved for
    // future opt-in; not used by any operator in this change.
    INSERTION_ORDER,
    // The operator outputs rows in a fixed order that must be preserved
    // (ORDER BY, TopK). Routes to the deterministic pairwise-merge path.
    FIXED_ORDER,
};

enum class PhysicalOperatorType : uint8_t {
    ALTER,
    AGGREGATE,
    AGGREGATE_FINALIZE,
    AGGREGATE_SCAN,
    ANALYZE,
    ATTACH_DATABASE,
    BATCH_INSERT,
    COPY_TO,
    COUNT_REL_TABLE,
    CREATE_GRAPH,
    CREATE_INDEX,
    CREATE_MACRO,
    CREATE_SEQUENCE,
    CREATE_TABLE,
    CREATE_TYPE,
    CROSS_PRODUCT,
    DETACH_DATABASE,
    DELETE_,
    DROP,
    DUMMY_SINK,
    DUMMY_SIMPLE_SINK,
    EMPTY_RESULT,
    EXPORT_DATABASE,
    EXTENSION_CLAUSE,
    FILTER,
    FLATTEN,
    HASH_JOIN_BUILD,
    HASH_JOIN_PROBE,
    IMPORT_DATABASE,
    INDEX_LOOKUP,
    INSERT,
    INTERSECT_BUILD,
    INTERSECT,
    INSTALL_EXTENSION,
    LIMIT,
    LOAD_EXTENSION,
    MERGE,
    MULTIPLICITY_REDUCER,
    PARTITIONER,
    PACKED_EXTEND,
    PACKED_FILTERED_COUNT,
    PATH_PROPERTY_PROBE,
    PRIMARY_KEY_SCAN_NODE_TABLE,
    PROJECTION,
    PROFILE,
    RECURSIVE_EXTEND,
    REL_DEGREE_TABLE,
    RESULT_COLLECTOR,
    SCAN_NODE_TABLE,
    SCAN_REL_TABLE,
    SEMI_MASKER,
    SET_PROPERTY,
    SKIP,
    STANDALONE_CALL,
    TABLE_FUNCTION_CALL,
    TOP_K,
    TOP_K_SCAN,
    TRANSACTION,
    ORDER_BY,
    ORDER_BY_MERGE,
    ORDER_BY_SCAN,
    UNION_ALL_SCAN,
    UNWIND,
    UNWIND_DEDUP,
    USE_DATABASE,
    USE_GRAPH,
    UNINSTALL_EXTENSION,
};

class PhysicalOperator;
struct PhysicalOperatorUtils {
    static std::string operatorToString(const PhysicalOperator* physicalOp);
    LBUG_API static std::string operatorTypeToString(PhysicalOperatorType operatorType);
};

struct OperatorMetrics {
    common::TimeMetric& executionTime;
    common::NumericMetric& numOutputTuple;

    OperatorMetrics(common::TimeMetric& executionTime, common::NumericMetric& numOutputTuple)
        : executionTime{executionTime}, numOutputTuple{numOutputTuple} {}
};

using physical_op_vector_t = std::vector<std::unique_ptr<PhysicalOperator>>;

class LBUG_API PhysicalOperator {
public:
    // Leaf operator
    PhysicalOperator(PhysicalOperatorType operatorType, physical_op_id id,
        std::unique_ptr<OPPrintInfo> printInfo)
        : id{id}, operatorType{operatorType}, resultSet(nullptr), printInfo{std::move(printInfo)} {}
    // Unary operator
    PhysicalOperator(PhysicalOperatorType operatorType, std::unique_ptr<PhysicalOperator> child,
        physical_op_id id, std::unique_ptr<OPPrintInfo> printInfo);
    // Binary operator
    PhysicalOperator(PhysicalOperatorType operatorType, std::unique_ptr<PhysicalOperator> left,
        std::unique_ptr<PhysicalOperator> right, physical_op_id id,
        std::unique_ptr<OPPrintInfo> printInfo);
    PhysicalOperator(PhysicalOperatorType operatorType, physical_op_vector_t children,
        physical_op_id id, std::unique_ptr<OPPrintInfo> printInfo);

    virtual ~PhysicalOperator() = default;

    physical_op_id getOperatorID() const { return id; }

    PhysicalOperatorType getOperatorType() const { return operatorType; }

    virtual bool isSource() const { return false; }
    virtual bool isSink() const { return false; }
    virtual bool isParallel() const { return true; }

    // Order-preservation metadata, used by PhysicalPlanUtil::getOrderPreservation
    // to walk the plan and decide which Arrow result-collector strategy to use.
    // Default is NO_ORDER (Ladybug makes no insertion-order guarantee).
    // See OrderPreservationType above for the meaning of each value.
    virtual OrderPreservationType operatorOrder() const { return OrderPreservationType::NO_ORDER; }
    virtual OrderPreservationType sourceOrder() const { return OrderPreservationType::NO_ORDER; }

    void addChild(std::unique_ptr<PhysicalOperator> op) { children.push_back(std::move(op)); }
    PhysicalOperator* getChild(common::idx_t idx) const { return children[idx].get(); }
    common::idx_t getNumChildren() const { return children.size(); }
    std::unique_ptr<PhysicalOperator> moveUnaryChild();

    // Global state is initialized once.
    void initGlobalState(ExecutionContext* context);
    // Local state is initialized for each thread.
    void initLocalState(ResultSet* resultSet, ExecutionContext* context);

    bool getNextTuple(ExecutionContext* context);

    virtual void finalize(ExecutionContext* context);

    std::unordered_map<std::string, std::string> getProfilerKeyValAttributes(
        common::Profiler& profiler) const;
    std::vector<std::string> getProfilerAttributes(common::Profiler& profiler) const;

    const OPPrintInfo* getPrintInfo() const { return printInfo.get(); }

    virtual std::unique_ptr<PhysicalOperator> copy() = 0;

    virtual double getProgress(ExecutionContext* context) const;

    template<class TARGET>
    TARGET* ptrCast() {
        return common::dynamic_cast_checked<TARGET*>(this);
    }
    template<class TARGET>
    const TARGET& constCast() {
        return common::dynamic_cast_checked<const TARGET&>(*this);
    }

protected:
    virtual void initGlobalStateInternal(ExecutionContext* /*context*/) {}
    virtual void initLocalStateInternal(ResultSet* /*resultSet_*/, ExecutionContext* /*context*/) {}
    // Return false if no more tuples to pull, otherwise return true
    virtual bool getNextTuplesInternal(ExecutionContext* context) = 0;

    std::string getTimeMetricKey() const { return "time-" + std::to_string(id); }
    std::string getNumTupleMetricKey() const { return "numTuple-" + std::to_string(id); }

    void registerProfilingMetrics(common::Profiler* profiler);

    double getExecutionTime(common::Profiler& profiler) const;
    uint64_t getNumOutputTuples(common::Profiler& profiler) const;

    virtual void finalizeInternal(ExecutionContext* /*context*/) {}

protected:
    physical_op_id id;
    std::unique_ptr<OperatorMetrics> metrics;
    PhysicalOperatorType operatorType;

    physical_op_vector_t children;
    ResultSet* resultSet;
    std::unique_ptr<OPPrintInfo> printInfo;
};

} // namespace processor
} // namespace lbug

#pragma once

#include "planner/operator/logical_plan.h"

namespace lbug {
namespace planner {

class LogicalPlanUtil {
public:
    static std::string encodeJoin(LogicalPlan& logicalPlan);

    // Returns true if the primary data-flow path from `op` downward contains an
    // ORDER BY that actually orders the rows arriving at a result collector.
    // Only the main data-flow child (child 0) is followed; side inputs (e.g. the
    // build side of a hash join) do not appear in the output row stream.
    // Operators that combine independent subplans (UNION_ALL, INTERSECT,
    // CROSS_PRODUCT, RECURSIVE_EXTEND) restart the ordering question: an
    // ORDER BY inside one arm does not make the combined result ordered.
    static bool hasOrderByOnDataPath(const LogicalOperator& op);

private:
    static std::string encode(LogicalOperator* logicalOperator);
    static void encodeRecursive(LogicalOperator* logicalOperator, std::string& encodeString);
    // Encode joins
    static void encodeCrossProduct(LogicalOperator* logicalOperator, std::string& encodeString);
    static void encodeIntersect(LogicalOperator* logicalOperator, std::string& encodeString);
    static void encodeHashJoin(LogicalOperator* logicalOperator, std::string& encodeString);
    static void encodeExtend(LogicalOperator* logicalOperator, std::string& encodeString);
    static void encodeScanNodeTable(LogicalOperator* logicalOperator, std::string& encodeString);
    // Encode filter
    static void encodeFilter(LogicalOperator* logicalOperator, std::string& encodedString);
};

} // namespace planner
} // namespace lbug

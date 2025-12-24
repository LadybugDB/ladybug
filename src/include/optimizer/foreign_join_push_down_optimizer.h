#pragma once

#include "logical_operator_visitor.h"
#include "main/client_context.h"
#include "planner/operator/logical_plan.h"

namespace lbug {
namespace optimizer {

/**
 * This optimizer detects graph patterns where all nodes and relationships are backed by
 * foreign tables (e.g., DuckDB, Postgres, SQLite) from the same database. When detected,
 * it rewrites the entire pattern into a single SQL JOIN query pushed down to the foreign
 * database.
 *
 * Pattern detected:
 *      HASH_JOIN (c._ID)
 *        ├── [FLATTEN]
 *        │     └── HASH_JOIN (a._ID)
 *        │           ├── EXTEND (a)-[b]->(c)
 *        │           │     └── SCAN_NODE_TABLE (a)
 *        │           └── TABLE_FUNCTION_CALL (a's SQL scan)
 *        └── TABLE_FUNCTION_CALL (c's SQL scan)
 *
 * Rewritten to:
 *      TABLE_FUNCTION_CALL (SQL JOIN query)
 *
 * Requirements for rewrite:
 * 1. Both node scans must be TABLE_FUNCTION_CALL with supportsPushDown = true
 * 2. The rel table must have a scanFunction (foreign-backed)
 * 3. All three tables must be from the same foreign database
 */
class ForeignJoinPushDownOptimizer : public LogicalOperatorVisitor {
public:
    explicit ForeignJoinPushDownOptimizer(main::ClientContext* context) : context{context} {}

    void rewrite(planner::LogicalPlan* plan);

private:
    std::shared_ptr<planner::LogicalOperator> visitOperator(
        const std::shared_ptr<planner::LogicalOperator>& op);

    std::shared_ptr<planner::LogicalOperator> visitHashJoinReplace(
        std::shared_ptr<planner::LogicalOperator> op) override;

    main::ClientContext* context;
};

} // namespace optimizer
} // namespace lbug

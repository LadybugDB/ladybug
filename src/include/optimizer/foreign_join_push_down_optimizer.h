#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "function/table/bind_data.h"
#include "logical_operator_visitor.h"
#include "main/client_context.h"
#include "planner/operator/logical_plan.h"

namespace lbug {
namespace optimizer {

/**
 * This optimizer detects graph patterns where all nodes and relationships are backed by
 * foreign tables (e.g., DuckDB, Postgres, SQLite, Iceberg, Unity Catalog) from the same
 * database. When detected, it rewrites the entire pattern into a single SQL query pushed
 * down to the foreign database.
 *
 * Supported patterns:
 *
 * 1. N-hop traversals (N >= 1), e.g. MATCH (a)-[r1]->(b)-[r2]->(c):
 *      HASH_JOIN ...
 *        ├── ... EXTEND (a)-[r1]->(b) ...
 *        ├── ... EXTEND (b)-[r2]->(c) ...
 *        └── TABLE_FUNCTION_CALL node scans (one per node)
 *    Rewritten to:
 *      TABLE_FUNCTION_CALL (SQL multi-way JOIN query)
 *    WHERE/HAVING-style FILTER predicates found anywhere inside the matched subtree
 *    (filters on node properties and filters on edge properties) are re-attached above
 *    the pushed scan so the regular filter push-down pass can fold the translatable
 *    ones into SQL WHERE clauses.
 *
 * 2. Aggregations over a pushed pattern, e.g. MATCH ... RETURN a.name, count(*):
 *    the AGGREGATE operator is folded into the pushed scan as GROUP BY. ORDER BY over
 *    a pushed scan is folded in as ORDER BY (and the ORDER_BY operator is dropped so
 *    later passes cannot emit duplicate or unresolvable sort keys).
 *
 * 3. Variable-length (N-hop, recursive) traversals, e.g. MATCH (a)-[e*1..4]->(b):
 *      HASH_JOIN (input _ID)
 *        ├── PATH_PROPERTY_PROBE
 *        │     └── RECURSIVE_EXTEND
 *        └── TABLE_FUNCTION_CALL (input node scan)
 *    Rewritten to:
 *      TABLE_FUNCTION_CALL (SQL WITH RECURSIVE query)
 *
 * Requirements for rewrite:
 * 1. Every node scan in the pattern must be a TABLE_FUNCTION_CALL with supportsPushDown.
 *    (Single-node scans of foreign tables are planned as table function calls.)
 * 2. Every rel table must have a scanFunction (foreign-backed).
 * 3. All tables must be from the same foreign database.
 * 4. Recursive pushdown additionally requires plain WALK semantics with a finite upper
 *    bound, a single directed (FWD/BWD) rel, no path-object outputs and no intermediate
 *    node predicate.
 */
class ForeignJoinPushDownOptimizer : public LogicalOperatorVisitor {
public:
    explicit ForeignJoinPushDownOptimizer(main::ClientContext* context) : context{context} {}

    void rewrite(planner::LogicalPlan* plan);

    // Bookkeeping for table function calls created by this optimizer, so enclosing
    // AGGREGATE / ORDER BY operators can be folded into the pushed SQL. Keyed by the
    // bind-data pointer (owned by the plan tree, stable for the duration of rewrite()).
    struct PushedScanInfo {
        // Query template containing exactly one "{}" placeholder for the select list.
        // The template never contains a WHERE clause: predicates travel as FILTER
        // operators and are assembled into WHERE by the scan's own SQL builder.
        std::string queryTemplate;
        // Select-list items ("<expr> AS <alias>") substituted for "{}".
        std::vector<std::string> selectItems;
        // Raw node/rel variable names usable as SQL table aliases (e.g. "a", "r1").
        std::unordered_set<std::string> tableAliases;
        // External ID column per table alias, for _ID references.
        std::unordered_map<std::string, std::string> idColumns;
        // SQL output alias per result-column unique name (for ORDER BY / outer refs).
        std::unordered_map<std::string, std::string> outputAliases;
        bool hasGroupBy = false;
        bool hasOrderBy = false;
    };

    void registerPushedScan(const function::TableFuncBindData* bindData, PushedScanInfo info) {
        pushedScans[bindData] = std::move(info);
    }

private:
    std::shared_ptr<planner::LogicalOperator> visitOperator(
        const std::shared_ptr<planner::LogicalOperator>& op);

    std::shared_ptr<planner::LogicalOperator> visitHashJoinReplace(
        std::shared_ptr<planner::LogicalOperator> op) override;

    std::shared_ptr<planner::LogicalOperator> visitAggregateReplace(
        std::shared_ptr<planner::LogicalOperator> op) override;

    std::shared_ptr<planner::LogicalOperator> visitOrderByReplace(
        std::shared_ptr<planner::LogicalOperator> op) override;

    // Top-down recursive pushdown along the root spine (see .cpp).
    bool tryRecursiveTopDown(planner::LogicalPlan* plan);

    std::unordered_map<const function::TableFuncBindData*, PushedScanInfo> pushedScans;

    main::ClientContext* context;
};

} // namespace optimizer
} // namespace lbug

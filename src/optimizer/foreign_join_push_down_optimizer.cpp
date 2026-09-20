#include "optimizer/foreign_join_push_down_optimizer.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <queue>
#include <unordered_set>

#include "binder/expression/aggregate_function_expression.h"
#include "binder/expression/literal_expression.h"
#include "binder/expression/property_expression.h"
#include "binder/expression/variable_expression.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/string_utils.h"
#include "common/types/types.h"
#include "function/gds/rec_joins.h"
#include "main/database_manager.h"
#include "planner/operator/extend/logical_extend.h"
#include "planner/operator/extend/logical_recursive_extend.h"
#include "planner/operator/logical_aggregate.h"
#include "planner/operator/logical_distinct.h"
#include "planner/operator/logical_filter.h"
#include "planner/operator/logical_flatten.h"
#include "planner/operator/logical_hash_join.h"
#include "planner/operator/logical_order_by.h"
#include "planner/operator/logical_path_property_probe.h"
#include "planner/operator/logical_projection.h"
#include "planner/operator/logical_table_function_call.h"
#include "planner/operator/scan/logical_scan_node_table.h"
#include <format>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;
using namespace lbug::catalog;

namespace lbug {
namespace optimizer {

void ForeignJoinPushDownOptimizer::rewrite(LogicalPlan* plan) {
    tryRecursiveTopDown(plan);
    visitOperator(plan->getLastOperator());
}

// Helper function to check if a logical operator is a TABLE_FUNCTION_CALL that supports pushdown
static bool isForeignTableFunctionCall(const LogicalOperator* op) {
    if (op->getOperatorType() != LogicalOperatorType::TABLE_FUNCTION_CALL) {
        return false;
    }
    auto& tableFuncCall = op->constCast<LogicalTableFunctionCall>();
    return tableFuncCall.getTableFunc().supportsPushDownFunc();
}

// Helper to check if a rel entry has foreign storage
static bool hasForeignScanFunction(const RelExpression* rel) {
    if (rel->getNumEntries() != 1) {
        return false;
    }
    auto relEntry = rel->getEntry(0)->ptrCast<RelGroupCatalogEntry>();
    return relEntry && relEntry->getScanFunction().has_value();
}

// Helper to get foreign database name from a node table entry
static std::string getNodeForeignDatabaseName(const NodeExpression* node,
    main::ClientContext* context) {
    if (!node || node->getNumEntries() != 1) {
        return "";
    }
    auto entry = node->getEntry(0);
    if (!entry) {
        return "";
    }
    std::string dbName;
    if (entry->getType() == CatalogEntryType::NODE_TABLE_ENTRY) {
        auto nodeEntry = entry->ptrCast<NodeTableCatalogEntry>();
        if (!nodeEntry) {
            return "";
        }
        dbName = nodeEntry->getForeignDatabaseName();
    } else if (entry->getType() == CatalogEntryType::FOREIGN_TABLE_ENTRY) {
        dbName = node->getDbName(entry);
    }
    if (dbName.empty()) {
        return "";
    }
    auto dbManager = main::DatabaseManager::Get(*context);
    if (!dbManager) {
        return "";
    }
    auto attachedDB = dbManager->getAttachedDatabase(dbName);
    if (!attachedDB) {
        return "";
    }
    return std::format("{}({})", dbName, attachedDB->getDBType());
}

// Helper to get foreign database name from a rel group entry
static std::string getRelForeignDatabaseName(const RelExpression* rel,
    main::ClientContext* context) {
    if (!rel || rel->getNumEntries() != 1) {
        return "";
    }
    auto entry = rel->getEntry(0);
    if (!entry) {
        return "";
    }
    auto relEntry = entry->ptrCast<RelGroupCatalogEntry>();
    if (!relEntry) {
        return "";
    }
    // First try the stored foreignDatabaseName. Stored names come in two
    // shapes: "db(TYPE)" from DDL-created rel tables (bind_ddl.cpp) and the
    // raw attached-db name from extension-registered groups. Normalize both
    // to the "db(TYPE)" format used by getNodeForeignDatabaseName so the
    // equality checks below work.
    auto storedName = relEntry->getForeignDatabaseName();
    if (!storedName.empty()) {
        auto rawName = storedName;
        auto parenPos = storedName.find('(');
        if (parenPos != std::string::npos) {
            rawName = storedName.substr(0, parenPos);
        }
        auto dbManager = main::DatabaseManager::Get(*context);
        if (!dbManager) {
            return storedName;
        }
        auto* attachedDB = dbManager->getAttachedDatabase(rawName);
        if (!attachedDB) {
            return storedName;
        }
        return std::format("{}({})", rawName, attachedDB->getDBType());
    }
    // For foreign rel tables, extract from storage
    auto storage = relEntry->getStorage();
    auto dotPos = storage.find('.');
    if (dotPos == std::string::npos) {
        return "";
    }
    auto dbName = storage.substr(0, dotPos);
    auto dbManager = main::DatabaseManager::Get(*context);
    if (!dbManager) {
        return "";
    }
    auto attachedDB = dbManager->getAttachedDatabase(dbName);
    if (!attachedDB) {
        return "";
    }
    return std::format("{}({})", dbName, attachedDB->getDBType());
}

static std::string stripIdentifierQuotes(const std::string& name) {
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
        return name.substr(1, name.size() - 2);
    }
    return name;
}

// Drop the catalog/schema qualifier: `catalog[.schema]."table"` -> `table`.
// Note: dots inside a quoted identifier are not handled; such names simply
// miss both lookups and surface as empty below.
static std::string unqualifyTableName(const std::string& tableName) {
    auto dotPos = tableName.rfind('.');
    auto unqualified = dotPos == std::string::npos ? tableName : tableName.substr(dotPos + 1);
    return stripIdentifierQuotes(unqualified);
}

// Helper to get column names from a foreign table. tableName may be a bare
// table name or a qualified `catalog[.schema].table` reference; the attached
// database scopes the lookup when qualification is present. Falls back to the
// unqualified table name so that attached databases from older extension
// builds (which only match bare names) keep working.
static std::vector<std::string> getForeignTableColumnNames(const std::string& dbName,
    const std::string& tableName, main::ClientContext* context) {
    if (dbName.empty() || tableName.empty() || !context) {
        return {};
    }
    auto dbManager = main::DatabaseManager::Get(*context);
    if (!dbManager) {
        return {};
    }
    auto attachedDB = dbManager->getAttachedDatabase(dbName);
    if (!attachedDB) {
        return {};
    }
    auto columnNames = attachedDB->getTableColumnNames(tableName);
    if (!columnNames.empty() || tableName.find('.') == std::string::npos) {
        return columnNames;
    }
    // Last resort for older extension builds. Note getTableColumnNames()
    // reports both "not found" and genuine errors as empty, so a qualified
    // miss retries against the attached database's default scope and could
    // bind a same-named table in a different schema. Accepted for backward
    // compatibility; current extension builds scope the qualified lookup first.
    return attachedDB->getTableColumnNames(unqualifyTableName(tableName));
}

// Endpoint-column convention shared with DuckDBCatalog::createForeignRelTable
// (extension): relationship endpoint columns carry src/dst/dest prefixes
// (e.g. src_id, dst, destination). Keep the two in sync. The prefixes are
// disjoint, so a column matches at most one side.
static bool isEndpointColumn(const std::string& lowerColumnName, bool wantSrc) {
    const bool isSrc = lowerColumnName.rfind("src", 0) == 0;
    const bool isDst =
        lowerColumnName.rfind("dst", 0) == 0 || lowerColumnName.rfind("dest", 0) == 0;
    return wantSrc ? isSrc : isDst;
}

static std::string sanitizeSQLAlias(std::string alias) {
    for (auto& ch : alias) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
            ch = '_';
        }
    }
    if (alias.empty() || std::isdigit(static_cast<unsigned char>(alias[0]))) {
        alias = "col_" + alias;
    }
    return alias;
}

// Extract the raw variable name carried by an expression when it denotes a
// node/rel reference: PropertyExpressions expose it directly; Variables bound
// to a single pattern (e.g. table-scan outputs named "_N_var.prop") parse it
// out of the unique name. Returns "" when no variable can be determined.
static std::string getExpressionRawVar(const Expression& expr) {
    if (expr.expressionType == ExpressionType::PROPERTY) {
        return expr.constCast<PropertyExpression>().getRawVariableName();
    }
    if (expr.expressionType == ExpressionType::VARIABLE) {
        auto uniqueName = expr.getUniqueName();
        // "_N_var[.prop]" -> "var". Anything else has no attributable variable.
        if (uniqueName.empty() || uniqueName[0] != '_') {
            return "";
        }
        auto end = uniqueName.find('.');
        auto prefix = end == std::string::npos ? uniqueName : uniqueName.substr(0, end);
        auto secondUnderscore = prefix.find('_', 1);
        if (secondUnderscore == std::string::npos || secondUnderscore + 1 >= prefix.size()) {
            return "";
        }
        return prefix.substr(secondUnderscore + 1);
    }
    return "";
}

// ---------------------------------------------------------------------------
// Expression -> SQL translation.
//
// Properties of pushed tables translate to "<tableAlias>.<column>" references,
// which resolve against the JOIN aliases of the generated query. Literals
// render as SQL constants. Anything else (parameters, function calls, CASE,
// subqueries, cross-variable arithmetic, ...) is untranslatable and must stay
// in a residual local operator. Parameters in particular must never be baked
// into pushed SQL: prepared statements re-execute the same plan with different
// parameter values.
// ---------------------------------------------------------------------------

struct SQLTranslationContext {
    // Table aliases available as SQL range variables (node and rel variables).
    std::unordered_set<std::string> tableAliases;
    // Fallback variable for property references that carry no variable name.
    // Recursive edge predicates are bound against the intermediate rel whose
    // variable name is empty, so they resolve to the rel alias when set.
    std::string implicitVar;
    // External ID column per table alias, for _ID references.
    std::unordered_map<std::string, std::string> idColumns;
    // SQL output alias per result-column unique name (for ORDER BY / GROUP BY
    // outputs that reference already-pushed select items).
    std::unordered_map<std::string, std::string> outputAliases;
};

static std::string renderLiteralSQL(const common::Value& value) {
    if (value.isNull()) {
        return "NULL";
    }
    switch (value.getDataType().getLogicalTypeID()) {
    case LogicalTypeID::BOOL:
        return value.getValue<bool>() ? "TRUE" : "FALSE";
    case LogicalTypeID::INT8:
        return std::to_string(static_cast<int64_t>(value.getValue<int8_t>()));
    case LogicalTypeID::INT16:
        return std::to_string(static_cast<int64_t>(value.getValue<int16_t>()));
    case LogicalTypeID::INT32:
        return std::to_string(static_cast<int64_t>(value.getValue<int32_t>()));
    case LogicalTypeID::INT64:
    case LogicalTypeID::SERIAL:
        return std::to_string(value.getValue<int64_t>());
    case LogicalTypeID::UINT8:
        return std::to_string(static_cast<uint64_t>(value.getValue<uint8_t>()));
    case LogicalTypeID::UINT16:
        return std::to_string(static_cast<uint64_t>(value.getValue<uint16_t>()));
    case LogicalTypeID::UINT32:
        return std::to_string(static_cast<uint64_t>(value.getValue<uint32_t>()));
    case LogicalTypeID::UINT64:
        return std::to_string(value.getValue<uint64_t>());
    case LogicalTypeID::FLOAT:
    case LogicalTypeID::DOUBLE:
        return value.toString();
    case LogicalTypeID::STRING: {
        auto str = value.getValue<std::string>();
        std::string out = "'";
        for (auto ch : str) {
            if (ch == '\'') {
                out += "''";
            } else {
                out += ch;
            }
        }
        out += "'";
        return out;
    }
    default:
        // Dates, timestamps, intervals, blobs, decimals and nested values have
        // dialect-sensitive literal syntax; leave them to local evaluation.
        return "";
    }
}

// Translate a value operand (property reference, output-column reference or
// constant). Returns "" when the operand cannot be pushed.
static std::string translateOperandToSQL(const Expression& expr, const SQLTranslationContext& ctx) {
    switch (expr.expressionType) {
    case ExpressionType::PROPERTY: {
        auto& prop = expr.constCast<PropertyExpression>();
        auto rawVar = prop.getRawVariableName();
        if (rawVar.empty()) {
            rawVar = ctx.implicitVar;
        }
        if (!ctx.tableAliases.contains(rawVar)) {
            // Not a pushed table; maybe a reference to an already-pushed output.
            auto outIt = ctx.outputAliases.find(expr.getUniqueName());
            if (outIt != ctx.outputAliases.end()) {
                return outIt->second;
            }
            return "";
        }
        auto propName = prop.getPropertyName();
        if (propName == InternalKeyword::ID) {
            auto idIt = ctx.idColumns.find(rawVar);
            if (idIt == ctx.idColumns.end()) {
                return "";
            }
            return std::format("{}.{}", rawVar, idIt->second);
        }
        return std::format("{}.{}", rawVar, propName);
    }
    case ExpressionType::VARIABLE: {
        auto outIt = ctx.outputAliases.find(expr.getUniqueName());
        if (outIt != ctx.outputAliases.end()) {
            return outIt->second;
        }
        return "";
    }
    case ExpressionType::LITERAL: {
        auto& literal = expr.constCast<LiteralExpression>();
        return renderLiteralSQL(literal.getValue());
    }
    default:
        return "";
    }
}

static std::string comparisonOpToSQL(ExpressionType type) {
    switch (type) {
    case ExpressionType::EQUALS:
        return "=";
    case ExpressionType::NOT_EQUALS:
        return "<>";
    case ExpressionType::GREATER_THAN:
        return ">";
    case ExpressionType::GREATER_THAN_EQUALS:
        return ">=";
    case ExpressionType::LESS_THAN:
        return "<";
    case ExpressionType::LESS_THAN_EQUALS:
        return "<=";
    default:
        return "";
    }
}

// Translate a boolean predicate. Returns "" when any part cannot be pushed.
static std::string translatePredicateToSQL(const Expression& expr,
    const SQLTranslationContext& ctx) {
    switch (expr.expressionType) {
    case ExpressionType::AND:
    case ExpressionType::OR: {
        if (expr.getNumChildren() != 2) {
            return "";
        }
        auto left = translatePredicateToSQL(*expr.getChild(0), ctx);
        auto right = translatePredicateToSQL(*expr.getChild(1), ctx);
        if (left.empty() || right.empty()) {
            return "";
        }
        auto op = expr.expressionType == ExpressionType::AND ? "AND" : "OR";
        return std::format("({} {} {})", left, op, right);
    }
    case ExpressionType::NOT: {
        if (expr.getNumChildren() != 1) {
            return "";
        }
        auto child = translatePredicateToSQL(*expr.getChild(0), ctx);
        if (child.empty()) {
            return "";
        }
        return std::format("(NOT {})", child);
    }
    case ExpressionType::IS_NULL:
    case ExpressionType::IS_NOT_NULL: {
        if (expr.getNumChildren() != 1) {
            return "";
        }
        auto child = translateOperandToSQL(*expr.getChild(0), ctx);
        if (child.empty()) {
            return "";
        }
        return std::format("{} {}", child,
            expr.expressionType == ExpressionType::IS_NULL ? "IS NULL" : "IS NOT NULL");
    }
    case ExpressionType::EQUALS:
    case ExpressionType::NOT_EQUALS:
    case ExpressionType::GREATER_THAN:
    case ExpressionType::GREATER_THAN_EQUALS:
    case ExpressionType::LESS_THAN:
    case ExpressionType::LESS_THAN_EQUALS: {
        if (expr.getNumChildren() != 2) {
            return "";
        }
        auto& leftExpr = *expr.getChild(0);
        auto& rightExpr = *expr.getChild(1);
        auto leftIsNull = leftExpr.expressionType == ExpressionType::LITERAL &&
                          leftExpr.constCast<LiteralExpression>().isNull();
        auto rightIsNull = rightExpr.expressionType == ExpressionType::LITERAL &&
                           rightExpr.constCast<LiteralExpression>().isNull();
        if (leftIsNull || rightIsNull) {
            // NULL comparisons only push as IS [NOT] NULL on equality.
            if (expr.expressionType != ExpressionType::EQUALS &&
                expr.expressionType != ExpressionType::NOT_EQUALS) {
                return "";
            }
            auto& other = leftIsNull ? rightExpr : leftExpr;
            auto operand = translateOperandToSQL(other, ctx);
            if (operand.empty()) {
                return "";
            }
            return std::format("{} {}", operand,
                expr.expressionType == ExpressionType::EQUALS ? "IS NULL" : "IS NOT NULL");
        }
        auto left = translateOperandToSQL(leftExpr, ctx);
        auto right = translateOperandToSQL(rightExpr, ctx);
        if (left.empty() || right.empty()) {
            return "";
        }
        return std::format("{} {} {}", left, comparisonOpToSQL(expr.expressionType), right);
    }
    default:
        return "";
    }
}

// ---------------------------------------------------------------------------
// N-hop chain matching.
//
// The collector walks a HASH_JOIN-rooted subtree and gathers every hop
// (EXTEND over a foreign rel), every foreign node scan (TABLE_FUNCTION_CALL)
// and every FILTER predicate, regardless of nesting. Supported nestings
// include the classic one-hop shape
//      HJ(bound) over [HJ over EXTEND + node scan] + node scan
// as well as multi-hop variants where extends chain on either join side.
// Anything else (projections, unions, cross products, recursive operators,
// ...) aborts the match and the plan is left untouched.
// ---------------------------------------------------------------------------

struct HopInfo {
    const LogicalExtend* extend = nullptr;
    std::string boundVar; // raw variable name of the bound node
    std::string nbrVar;   // raw variable name of the neighbour node
    std::string relVar;   // raw variable name of the relationship
    ExtendDirection direction = ExtendDirection::FWD;
    std::string relTable;   // qualified foreign table reference
    std::string boundEPCol; // rel endpoint column joining the bound node
    std::string nbrEPCol;   // rel endpoint column joining the neighbour node
};

// A variable-length (N-hop, recursive) traversal collected from a
// PATH_PROPERTY_PROBE over a RECURSIVE_EXTEND. At most one per matched
// subtree (mixed fixed/variable patterns stay local). Tables resolve through
// the same node-scan mapping as fixed hops.
struct RecursiveHop {
    const LogicalRecursiveExtend* extend = nullptr;
    std::string inputVar;  // raw variable starting the traversal (bound)
    std::string outputVar; // raw variable ending the traversal (neighbour)
    std::string relVar;
    std::shared_ptr<RelExpression> rel; // probe's rel (owns src/dst nodes)
    ExtendDirection direction = ExtendDirection::FWD;
    uint16_t lowerBound = 1;
    uint16_t upperBound = 1;
    // Resolved during finalization.
    std::string inputTable;
    std::string outputTable;
    std::string relTable;
    std::string inputIDCol;
    std::string outputIDCol;
    std::string srcEPCol;      // rel endpoint column on the traversal-start side
    std::string dstEPCol;      // rel endpoint column on the traversal-end side
    std::string edgeFilterSQL; // baked into anchor + recursive term
};

struct ChainMatchInfo {
    std::vector<HopInfo> hops;
    std::optional<RecursiveHop> recursive;
    // Raw node variable -> qualified foreign table reference (from scan descriptions).
    std::unordered_map<std::string, std::string> nodeTables;
    // Raw node variable -> bound NodeExpression (first occurrence in hop order).
    std::unordered_map<std::string, std::shared_ptr<NodeExpression>> nodeExprs;
    // FILTER predicates found anywhere inside the matched subtree.
    std::vector<std::shared_ptr<Expression>> filterPredicates;
    // Variables of LOCAL node scans (extend inputs); must be chain members.
    std::unordered_set<std::string> localScanVars;
    // Variables each hop touches (for connectivity validation).
    std::string dbName;    // normalized "db(TYPE)"
    std::string dbRawName; // "db"
    const Schema* outputSchema = nullptr;
    const LogicalTableFunctionCall* representativeTF = nullptr;
};

class ChainCollector {
public:
    ChainCollector(main::ClientContext* context) : context{context} {}

    bool collect(const std::shared_ptr<LogicalOperator>& op) {
        collectOp(op.get());
        return !failed;
    }

    ChainMatchInfo chain;
    bool failed = false;

private:
    void fail() { failed = true; }

    // Unwrap FLATTEN/FILTER operators, saving filter predicates for
    // re-attachment above the pushed scan. Returns the first inner operator
    // that is neither (or nullptr on malformed input).
    const LogicalOperator* unwrapFilters(const LogicalOperator* op) {
        auto current = op;
        while (
            current != nullptr && (current->getOperatorType() == LogicalOperatorType::FLATTEN ||
                                      current->getOperatorType() == LogicalOperatorType::FILTER)) {
            if (current->getOperatorType() == LogicalOperatorType::FILTER) {
                chain.filterPredicates.push_back(
                    current->constPtrCast<LogicalFilter>()->getPredicate());
            }
            if (current->getNumChildren() < 1) {
                fail();
                return nullptr;
            }
            current = current->getChild(0).get();
        }
        return current;
    }

    void collectOp(const LogicalOperator* op) {
        if (failed || op == nullptr) {
            fail();
            return;
        }
        // Unwrap flattening and filtering operators; filters are re-attached
        // above the pushed scan (translatable ones fold into SQL WHERE later).
        auto current = unwrapFilters(op);
        if (failed || current == nullptr) {
            fail();
            return;
        }
        switch (current->getOperatorType()) {
        case LogicalOperatorType::HASH_JOIN: {
            auto& hashJoin = current->constCast<LogicalHashJoin>();
            if (hashJoin.getJoinType() != JoinType::INNER || hashJoin.hasMark()) {
                fail();
                return;
            }
            if (current->getNumChildren() < 2) {
                fail();
                return;
            }
            // Join keys identify which node each side provides, e.g. ("a._ID",
            // "a._ID") means both sides carry node a.
            std::string pVar, bVar;
            auto conditions = hashJoin.getJoinConditions();
            if (!conditions.empty()) {
                if (conditions[0].first) {
                    pVar = getExpressionRawVar(*conditions[0].first);
                }
                if (conditions[0].second) {
                    bVar = getExpressionRawVar(*conditions[0].second);
                }
            }
            // Join sides may be wrapped in FILTER/FLATTEN (e.g. node filters
            // above a scan). Unwrap them here so the join-key variable stays
            // available for table mapping.
            auto probe = unwrapFilters(current->getChild(0).get());
            if (failed) {
                return;
            }
            auto build = unwrapFilters(current->getChild(1).get());
            if (failed) {
                return;
            }
            // A foreign node scan on either side maps its table to the join key
            // variable; any other shape recurses as more of the chain.
            if (probe != nullptr && isForeignTableFunctionCall(probe)) {
                mapNodeScan(probe->constPtrCast<LogicalTableFunctionCall>(), pVar);
            } else if (probe != nullptr &&
                       probe->getOperatorType() == LogicalOperatorType::SCAN_NODE_TABLE) {
                chain.localScanVars.insert(
                    getExpressionRawVar(*probe->constPtrCast<LogicalScanNodeTable>()->getNodeID()));
            } else {
                collectOp(probe);
            }
            if (failed) {
                return;
            }
            if (build != nullptr && isForeignTableFunctionCall(build)) {
                mapNodeScan(build->constPtrCast<LogicalTableFunctionCall>(), bVar);
            } else if (build != nullptr &&
                       build->getOperatorType() == LogicalOperatorType::SCAN_NODE_TABLE) {
                chain.localScanVars.insert(
                    getExpressionRawVar(*build->constPtrCast<LogicalScanNodeTable>()->getNodeID()));
            } else {
                collectOp(build);
            }
            return;
        }
        case LogicalOperatorType::EXTEND: {
            auto extend = current->constPtrCast<LogicalExtend>();
            auto rel = extend->getRel();
            auto boundNode = extend->getBoundNode();
            auto nbrNode = extend->getNbrNode();
            if (!rel || !boundNode || !nbrNode || !hasForeignScanFunction(rel.get())) {
                fail();
                return;
            }
            auto srcDb = getNodeForeignDatabaseName(boundNode.get(), context);
            auto dstDb = getNodeForeignDatabaseName(nbrNode.get(), context);
            auto relDb = getRelForeignDatabaseName(rel.get(), context);
            if (srcDb.empty() || dstDb.empty() || relDb.empty() || srcDb != dstDb ||
                srcDb != relDb) {
                fail();
                return;
            }
            if (chain.dbName.empty()) {
                chain.dbName = srcDb;
                auto parenPos = srcDb.find('(');
                chain.dbRawName = parenPos == std::string::npos ? srcDb : srcDb.substr(0, parenPos);
            } else if (chain.dbName != srcDb) {
                // Cross-database pattern: each side must stay local.
                fail();
                return;
            }
            HopInfo hop;
            hop.extend = extend;
            hop.boundVar = boundNode->getVariableName();
            hop.nbrVar = nbrNode->getVariableName();
            hop.relVar = rel->getVariableName();
            hop.direction = extend->getDirection();
            if (hop.boundVar.empty() || hop.nbrVar.empty() || hop.relVar.empty()) {
                fail();
                return;
            }
            if (!chain.nodeExprs.contains(hop.boundVar)) {
                chain.nodeExprs[hop.boundVar] = boundNode;
            }
            if (!chain.nodeExprs.contains(hop.nbrVar)) {
                chain.nodeExprs[hop.nbrVar] = nbrNode;
            }
            chain.hops.push_back(std::move(hop));
            if (current->getNumChildren() < 1) {
                fail();
                return;
            }
            auto extendChild = current->getChild(0).get();
            if (extendChild != nullptr &&
                extendChild->getOperatorType() == LogicalOperatorType::SCAN_NODE_TABLE) {
                // The extend's bound-node input; must be the bound variable so the
                // collected chain stays a faithful picture of the pattern.
                auto& scan = extendChild->constCast<LogicalScanNodeTable>();
                auto scanVar = getExpressionRawVar(*scan.getNodeID());
                if (scanVar != chain.hops.back().boundVar) {
                    fail();
                    return;
                }
                chain.localScanVars.insert(scanVar);
                return;
            }
            if (extendChild != nullptr && isForeignTableFunctionCall(extendChild)) {
                // The bound node is scanned directly under the extend.
                mapNodeScan(extendChild->constPtrCast<LogicalTableFunctionCall>(),
                    chain.hops.back().boundVar);
                return;
            }
            collectOp(extendChild);
            return;
        }
        case LogicalOperatorType::SCAN_NODE_TABLE: {
            auto& scan = current->constCast<LogicalScanNodeTable>();
            chain.localScanVars.insert(getExpressionRawVar(*scan.getNodeID()));
            return;
        }
        case LogicalOperatorType::PATH_PROPERTY_PROBE: {
            // Variable-length core: probe over a recursive extend. The
            // probe's side inputs vanish with it; only the outputs matter.
            auto& probe = current->constCast<LogicalPathPropertyProbe>();
            if (current->getNumChildren() < 1 ||
                current->getChild(0)->getOperatorType() != LogicalOperatorType::RECURSIVE_EXTEND) {
                fail();
                return;
            }
            if (chain.recursive.has_value() || !chain.hops.empty()) {
                // One recursive hop per subtree; no mixing with fixed hops.
                fail();
                return;
            }
            auto recursive = current->getChild(0)->constPtrCast<LogicalRecursiveExtend>();
            auto& bindData = recursive->getBindData();
            if (bindData.semantic != PathSemantic::WALK || bindData.weightPropertyExpr != nullptr) {
                fail();
                return;
            }
            static const std::unordered_set<std::string> kWalkFunctions = {"VAR_LEN_JOINS",
                "VARLENJOINS", "VARIABLE_LENGTH_JOINS"};
            auto functionName = recursive->getFunction().getFunctionName();
            StringUtils::toUpper(functionName);
            if (!kWalkFunctions.contains(functionName)) {
                fail();
                return;
            }
            if (bindData.extendDirection != ExtendDirection::FWD &&
                bindData.extendDirection != ExtendDirection::BWD) {
                fail();
                return;
            }
            if (recursive->hasNodePredicate() || recursive->getLimitNum() != INVALID_LIMIT) {
                fail();
                return;
            }
            if (bindData.upperBound == 0 || bindData.upperBound == UINT16_MAX) {
                fail();
                return;
            }
            if (bindData.lowerBound > bindData.upperBound) {
                fail();
                return;
            }
            if (recursive->getNumChildren() != 0) {
                fail();
                return;
            }
            auto inputNode = dynamic_cast<const NodeExpression*>(bindData.nodeInput.get());
            auto outputNode = dynamic_cast<const NodeExpression*>(bindData.nodeOutput.get());
            if (inputNode == nullptr || outputNode == nullptr) {
                fail();
                return;
            }
            auto rel = probe.getRel();
            if (!rel || rel->getNumEntries() != 1 || !hasForeignScanFunction(rel.get())) {
                fail();
                return;
            }
            auto recursiveInfo = rel->getRecursiveInfo();
            if (recursiveInfo != nullptr && recursiveInfo->nodePredicate != nullptr) {
                fail();
                return;
            }
            RecursiveHop hop;
            hop.extend = recursive;
            hop.inputVar = inputNode->getVariableName();
            hop.outputVar = outputNode->getVariableName();
            hop.relVar = rel->getVariableName();
            hop.rel = rel;
            hop.direction = bindData.extendDirection;
            hop.lowerBound = bindData.lowerBound;
            hop.upperBound = bindData.upperBound;
            if (hop.inputVar.empty() || hop.outputVar.empty() || hop.relVar.empty() ||
                hop.inputVar == hop.outputVar) {
                fail();
                return;
            }
            chain.recursive = std::move(hop);
            return;
        }
        default:
            // TABLE_FUNCTION_CALL roots (single node scans), projections,
            // unions, recursive operators and everything else: not a chain.
            fail();
            return;
        }
    }

    void mapNodeScan(const LogicalTableFunctionCall* tf, const std::string& var) {
        if (tf == nullptr || var.empty()) {
            fail();
            return;
        }
        auto table = extractTableName(tf->getBindData()->getDescription());
        if (table.empty()) {
            fail();
            return;
        }
        auto it = chain.nodeTables.find(var);
        if (it != chain.nodeTables.end()) {
            if (it->second != table) {
                // Same variable scanned from two different tables: not pushable.
                fail();
            }
            return;
        }
        chain.nodeTables[var] = table;
        if (chain.representativeTF == nullptr) {
            chain.representativeTF = tf;
        }
    }

    // Extract the table reference from a scan description ("... FROM <table> ...").
    static std::string extractTableName(const std::string& desc) {
        auto fromPos = desc.find("FROM ");
        if (fromPos == std::string::npos) {
            return "";
        }
        auto tableName = desc.substr(fromPos + 5);
        auto spacePos = tableName.find(' ');
        if (spacePos != std::string::npos) {
            tableName = tableName.substr(0, spacePos);
        }
        return tableName;
    }

    main::ClientContext* context;
};

// Resolve the node-table ID column without assuming column order: the bound
// catalog entry's primary key is the single source of truth; only fall back
// to a foreign lookup when the entry carries no primary key.
static std::string resolveNodeIDColumn(const NodeExpression* node, const std::string& tableName,
    const std::string& dbRawName, main::ClientContext* context) {
    if (node && node->getNumEntries() == 1) {
        if (auto entry = node->getEntry(0);
            entry && entry->getType() == CatalogEntryType::NODE_TABLE_ENTRY) {
            if (auto nodeEntry = entry->ptrCast<NodeTableCatalogEntry>(); nodeEntry) {
                auto pkName = nodeEntry->getPrimaryKeyName();
                if (!pkName.empty()) {
                    return pkName;
                }
            }
        }
    }
    auto columnNames = getForeignTableColumnNames(dbRawName, tableName, context);
    if (columnNames.empty()) {
        return std::string{InternalKeyword::ID};
    }
    for (auto& column : columnNames) {
        auto lowerCol = column;
        common::StringUtils::toLower(lowerCol);
        if (lowerCol == "id") {
            return column;
        }
    }
    return columnNames[0];
}

// Resolve (boundEndpointCol, nbrEndpointCol) for one hop: prefer endpoint
// columns identified by the src/dst naming convention and orient them by the
// planned extend direction (bound == src iff FWD).
static std::pair<std::string, std::string> resolveHopEndpointColumns(const std::string& dbRawName,
    const std::string& relTable, ExtendDirection direction, main::ClientContext* context) {
    auto tableColumnNames = getForeignTableColumnNames(dbRawName, relTable, context);
    if (tableColumnNames.size() < 2) {
        throw RuntimeException(std::format(
            "Foreign join push down optimizer: unable to retrieve column names for table '{}.{}', "
            "got {} columns but need at least 2 for join",
            dbRawName, relTable, tableColumnNames.size()));
    }
    std::string firstCol = tableColumnNames[0];
    std::string secondCol = tableColumnNames[1];
    auto findEndpointColumn = [&](bool wantSrc) -> std::string {
        for (auto& column : tableColumnNames) {
            auto lowerCol = column;
            common::StringUtils::toLower(lowerCol);
            if (isEndpointColumn(lowerCol, wantSrc)) {
                return column;
            }
        }
        return "";
    };
    auto srcCol = findEndpointColumn(true /* wantSrc */);
    auto dstCol = findEndpointColumn(false /* wantSrc */);
    // All-or-nothing: a lone prefix match without its counterpart is ambiguous,
    // so fall back to ordinal position for both columns.
    if (!srcCol.empty() && !dstCol.empty()) {
        firstCol = srcCol;
        secondCol = dstCol;
    }
    if (direction == ExtendDirection::FWD) {
        return {firstCol, secondCol};
    }
    return {secondCol, firstCol};
}

// Derive a qualified rel-table reference by reusing the catalog/schema prefix
// of a node-table reference from the same database.
static std::string qualifyRelTable(const std::string& nodeTableRef, const std::string& relStorage) {
    auto dotPos = relStorage.find('.');
    if (dotPos == std::string::npos) {
        return relStorage;
    }
    auto relName = relStorage.substr(dotPos + 1);
    auto prefixEnd = nodeTableRef.rfind('.');
    if (prefixEnd == std::string::npos) {
        return relName;
    }
    return nodeTableRef.substr(0, prefixEnd + 1) + relName;
}

struct JoinQueryInfo {
    std::string query;
    std::vector<std::string> columnNames;
    std::vector<std::string> displayNames;
};

// Validate a collected recursive hop and resolve table/column references.
// Returns false when the pattern must stay local.
static bool finalizeRecursiveChain(ChainMatchInfo& chain, main::ClientContext* context) {
    auto& hop = chain.recursive.value();
    if (chain.representativeTF == nullptr) {
        return false;
    }
    if (!chain.nodeTables.contains(hop.inputVar) || !chain.nodeTables.contains(hop.outputVar)) {
        return false;
    }
    // Local scans inside the subtree may only feed the traversal input.
    for (auto& var : chain.localScanVars) {
        if (var != hop.inputVar) {
            return false;
        }
    }
    auto rel = hop.rel;
    auto srcNode = rel->getSrcNode();
    auto dstNode = rel->getDstNode();
    if (!srcNode || !dstNode) {
        return false;
    }
    auto srcDb = getNodeForeignDatabaseName(srcNode.get(), context);
    auto dstDb = getNodeForeignDatabaseName(dstNode.get(), context);
    auto relDb = getRelForeignDatabaseName(rel.get(), context);
    if (srcDb.empty() || srcDb != dstDb || srcDb != relDb) {
        return false;
    }
    chain.dbName = srcDb;
    auto parenPos = srcDb.find('(');
    chain.dbRawName = parenPos == std::string::npos ? srcDb : srcDb.substr(0, parenPos);
    hop.inputTable = chain.nodeTables[hop.inputVar];
    hop.outputTable = chain.nodeTables[hop.outputVar];
    auto relEntry = rel->getEntry(0)->ptrCast<RelGroupCatalogEntry>();
    hop.relTable = qualifyRelTable(hop.inputTable, relEntry->getStorage());
    if (hop.relTable.empty()) {
        return false;
    }
    const NodeExpression* inputNode =
        srcNode->getVariableName() == hop.inputVar ? srcNode.get() : dstNode.get();
    const NodeExpression* outputNode =
        srcNode->getVariableName() == hop.outputVar ? srcNode.get() : dstNode.get();
    if (inputNode->getVariableName() != hop.inputVar ||
        outputNode->getVariableName() != hop.outputVar) {
        return false;
    }
    hop.inputIDCol = resolveNodeIDColumn(inputNode, hop.inputTable, chain.dbRawName, context);
    hop.outputIDCol = resolveNodeIDColumn(outputNode, hop.outputTable, chain.dbRawName, context);
    try {
        auto [boundEP, nbrEP] =
            resolveHopEndpointColumns(chain.dbRawName, hop.relTable, hop.direction, context);
        // resolveHopEndpointColumns orients bound->first; here bound == input.
        hop.srcEPCol = boundEP;
        hop.dstEPCol = nbrEP;
    } catch (RuntimeException&) {
        return false;
    }
    // Edge predicates bake into the CTE (rel columns are not path outputs,
    // so no residual filter could evaluate them).
    auto recursiveInfo = rel->getRecursiveInfo();
    std::shared_ptr<Expression> edgePred =
        recursiveInfo != nullptr ? recursiveInfo->relPredicate : nullptr;
    if (edgePred != nullptr) {
        SQLTranslationContext edgeCtx;
        // Anchor and recursive member share the rel alias (separate scopes).
        edgeCtx.tableAliases.insert(hop.relVar);
        edgeCtx.implicitVar = hop.relVar;
        hop.edgeFilterSQL = translatePredicateToSQL(*edgePred, edgeCtx);
        if (hop.edgeFilterSQL.empty()) {
            return false;
        }
    }
    return true;
}

// Validate a collected chain and resolve table/column references. Returns
// false when the pattern must stay local.
static bool finalizeChain(ChainMatchInfo& chain, main::ClientContext* context) {
    if (chain.recursive.has_value()) {
        return finalizeRecursiveChain(chain, context);
    }
    if (chain.hops.empty() || chain.dbName.empty() || chain.dbRawName.empty() ||
        chain.representativeTF == nullptr) {
        return false;
    }
    // Every hop endpoint must come from a foreign node scan.
    for (auto& hop : chain.hops) {
        if (!chain.nodeTables.contains(hop.boundVar) || !chain.nodeTables.contains(hop.nbrVar)) {
            return false;
        }
        if (!chain.nodeExprs.contains(hop.boundVar) || !chain.nodeExprs.contains(hop.nbrVar)) {
            return false;
        }
    }
    // Local scans inside the subtree must belong to the chain.
    for (auto& var : chain.localScanVars) {
        if (!chain.nodeTables.contains(var)) {
            return false;
        }
    }
    // The hops must form a single connected pattern; disjoint patterns must
    // not merge into one (cross-join) SQL query.
    std::unordered_map<std::string, std::string> parent;
    std::function<std::string(const std::string&)> find = [&](const std::string& v) -> std::string {
        auto it = parent.find(v);
        if (it == parent.end()) {
            parent[v] = v;
            return v;
        }
        if (it->second == v) {
            return v;
        }
        return it->second = find(it->second);
    };
    for (auto& hop : chain.hops) {
        auto pb = find(hop.boundVar);
        auto pn = find(hop.nbrVar);
        if (pb != pn) {
            parent[pb] = pn;
        }
    }
    auto root = find(chain.hops[0].boundVar);
    for (auto& hop : chain.hops) {
        if (find(hop.boundVar) != root || find(hop.nbrVar) != root) {
            return false;
        }
    }
    // Resolve the rel-table reference and endpoint columns per hop.
    for (auto& hop : chain.hops) {
        auto rel = hop.extend->getRel();
        auto relEntry = rel->getEntry(0)->ptrCast<RelGroupCatalogEntry>();
        std::string relStorage = relEntry->getStorage();
        hop.relTable = qualifyRelTable(chain.nodeTables[hop.boundVar], relStorage);
        if (hop.relTable.empty()) {
            return false;
        }
        try {
            auto [boundEP, nbrEP] =
                resolveHopEndpointColumns(chain.dbRawName, hop.relTable, hop.direction, context);
            hop.boundEPCol = boundEP;
            hop.nbrEPCol = nbrEP;
        } catch (RuntimeException&) {
            return false;
        }
    }
    return true;
}

// Build the SQL join query string and collect column names for result mapping.
// Hops are emitted in BFS order from the first hop's bound node so every JOIN
// has its left side already bound. Already-emitted neighbours contribute an
// extra ON conjunct (cycles/diamonds stay correct inner joins).
static JoinQueryInfo buildChainJoinQuery(ChainMatchInfo& chain,
    const expression_vector& outputColumns,
    const std::unordered_map<std::string, std::string>& nodeIDCols, main::ClientContext* context) {
    (void)context;
    // Hop adjacency: variable -> hop indices touching it.
    std::unordered_map<std::string, std::vector<size_t>> varHops;
    for (size_t i = 0; i < chain.hops.size(); i++) {
        varHops[chain.hops[i].boundVar].push_back(i);
        varHops[chain.hops[i].nbrVar].push_back(i);
    }
    std::unordered_set<std::string> emitted;
    std::vector<bool> hopEmitted(chain.hops.size(), false);
    std::string fromClause;
    std::string joinClause;
    std::queue<std::string> frontier;
    auto startVar = chain.hops[0].boundVar;
    fromClause = std::format("FROM {} {}", chain.nodeTables[startVar], startVar);
    emitted.insert(startVar);
    frontier.push(startVar);
    auto idColOf = [&](const std::string& var) -> std::string {
        auto it = nodeIDCols.find(var);
        return it == nodeIDCols.end() ? std::string{InternalKeyword::ID} : it->second;
    };
    while (!frontier.empty()) {
        auto var = frontier.front();
        frontier.pop();
        for (auto hopIdx : varHops[var]) {
            if (hopEmitted[hopIdx]) {
                continue;
            }
            hopEmitted[hopIdx] = true;
            auto& hop = chain.hops[hopIdx];
            // Orient the hop so `entry` is the already-emitted side.
            bool entryIsBound = (var == hop.boundVar);
            // Self-loops (bound == nbr) still emit both conditions against the
            // single range variable.
            auto entryVar = entryIsBound ? hop.boundVar : hop.nbrVar;
            auto otherVar = entryIsBound ? hop.nbrVar : hop.boundVar;
            auto entryEP = entryIsBound ? hop.boundEPCol : hop.nbrEPCol;
            auto otherEP = entryIsBound ? hop.nbrEPCol : hop.boundEPCol;
            std::string onClause =
                std::format("{}.{} = {}.{}", entryVar, idColOf(entryVar), hop.relVar, entryEP);
            joinClause += std::format(" JOIN {} {} ON {}", hop.relTable, hop.relVar, onClause);
            if (otherVar == entryVar) {
                // Self-loop: second condition on the same range variable.
                joinClause += std::format(" AND {}.{} = {}.{}", hop.relVar, otherEP, otherVar,
                    idColOf(otherVar));
            } else if (!emitted.contains(otherVar)) {
                joinClause +=
                    std::format(" JOIN {} {} ON {}.{} = {}.{}", chain.nodeTables[otherVar],
                        otherVar, hop.relVar, otherEP, otherVar, idColOf(otherVar));
                emitted.insert(otherVar);
                frontier.push(otherVar);
            } else {
                joinClause += std::format(" AND {}.{} = {}.{}", hop.relVar, otherEP, otherVar,
                    idColOf(otherVar));
            }
        }
    }
    // Safety net: any hop missed by BFS (should not happen after the
    // connectivity check) aborts the query build.
    for (auto done : hopEmitted) {
        if (!done) {
            return {};
        }
    }

    // Build SELECT items from output columns and collect column names.
    // Keep result column names SQL-safe while using display names that
    // preserve the user's labels (display names double as "<table>.<column>"
    // references for later filter/order pushdown).
    std::vector<std::string> columnNames;
    std::vector<std::string> displayNames;
    // Only variables bound by the emitted FROM/JOIN clauses may be
    // referenced: node tables merely seen in the subtree (but not part of
    // any hop) have no range variable and must abort the pushdown.
    std::unordered_set<std::string> knownVars = emitted;
    for (auto& hop : chain.hops) {
        knownVars.insert(hop.relVar);
    }
    for (auto& col : outputColumns) {
        std::string colExpr;
        std::string colName;
        std::string displayName;
        if (col->expressionType == ExpressionType::PROPERTY) {
            auto& prop = col->constCast<PropertyExpression>();
            auto rawVarName = prop.getRawVariableName();
            if (!knownVars.contains(rawVarName)) {
                return {};
            }
            auto propName = prop.getPropertyName();
            if (propName == InternalKeyword::ID) {
                colExpr = std::format("{}.{}", rawVarName, idColOf(rawVarName));
            } else {
                colExpr = std::format("{}.{}", rawVarName, propName);
            }
            colName = sanitizeSQLAlias(std::format("{}_{}", rawVarName, propName));
            displayName = std::format("{}.{}", rawVarName, propName);
        } else {
            // For non-property expressions, parse the unique name to extract
            // table alias and column ("_N_varname.columnname" ->
            // "varname.columnname").
            auto uniqueName = col->getUniqueName();
            auto dotPos = uniqueName.find('.');
            if (dotPos != std::string::npos) {
                auto prefix = uniqueName.substr(0, dotPos);
                auto colNamePart = uniqueName.substr(dotPos + 1);
                auto underscorePos = prefix.find('_', 1);
                if (underscorePos != std::string::npos) {
                    auto rawVar = prefix.substr(underscorePos + 1);
                    if (!knownVars.contains(rawVar)) {
                        return {};
                    }
                    colExpr = std::format("{}.{}", rawVar, colNamePart);
                    colName = sanitizeSQLAlias(std::format("{}_{}", rawVar, colNamePart));
                    displayName = std::format("{}.{}", rawVar, colNamePart);
                } else {
                    return {};
                }
            } else {
                return {};
            }
        }
        columnNames.push_back(std::format("{} AS {}", colExpr, colName));
        displayNames.push_back(displayName);
    }
    std::string query = std::format("SELECT {{}} {} {}", fromClause, joinClause);
    return {std::move(query), std::move(columnNames), std::move(displayNames)};
}

// Collect the output columns for a pushed scan: the operator scope's canonical
// expressions plus every pattern property (so filters re-attached above the
// scan stay evaluable) plus every property referenced by pushed-down filter
// predicates.
static expression_vector collectPushedOutputColumns(const Schema* schema,
    const std::vector<std::shared_ptr<NodeOrRelExpression>>& patterns,
    const std::vector<std::shared_ptr<Expression>>& filterPredicates) {
    auto allColumns = schema->getExpressionsInScope();
    expression_vector outputColumns;
    std::unordered_set<std::string> outputColumnNames;
    std::unordered_set<std::string> canonicalVarProps;

    auto appendOutputColumn = [&](const std::shared_ptr<Expression>& column) {
        if (!outputColumnNames.insert(column->getUniqueName()).second) {
            return;
        }
        outputColumns.push_back(column);
    };

    auto extractCanonicalVarProp = [](const std::string& uniqueName) -> std::string {
        // "_N_var.prop" -> "var.prop"
        if (uniqueName.empty() || uniqueName[0] != '_') {
            return "";
        }
        auto dotPos = uniqueName.find('.');
        if (dotPos == std::string::npos) {
            return "";
        }
        auto prefix = uniqueName.substr(0, dotPos); // "_N_var"
        auto secondUnderscore = prefix.find('_', 1);
        if (secondUnderscore == std::string::npos || secondUnderscore + 1 >= prefix.size()) {
            return "";
        }
        auto rawVar = prefix.substr(secondUnderscore + 1);
        auto prop = uniqueName.substr(dotPos + 1);
        if (rawVar.empty() || prop.empty()) {
            return "";
        }
        return rawVar + "." + prop;
    };

    for (auto& col : allColumns) {
        auto canonical = extractCanonicalVarProp(col->getUniqueName());
        if (!canonical.empty()) {
            canonicalVarProps.insert(canonical);
        }
    }

    auto isCanonicalOrStandalone = [&](const std::string& uniqueName) {
        if (uniqueName.empty() || uniqueName[0] == '_') {
            return true;
        }
        // "var.prop" helper column; skip if canonical "_N_var.prop" exists.
        return !canonicalVarProps.contains(uniqueName);
    };

    auto hasLowercaseID = [&](const std::string& uniqueName) {
        static constexpr auto internalIDSuffix = "._ID";
        static constexpr auto suffixLen = std::char_traits<char>::length(internalIDSuffix);
        if (uniqueName.size() <= suffixLen) {
            return false;
        }
        if (uniqueName.rfind(internalIDSuffix) != uniqueName.size() - suffixLen) {
            return false;
        }
        auto lowercaseID = uniqueName.substr(0, uniqueName.size() - suffixLen);
        lowercaseID += ".id";
        for (auto& expr : allColumns) {
            if (expr->getUniqueName() == lowercaseID) {
                return true;
            }
        }
        return false;
    };

    for (auto& col : allColumns) {
        auto uniqueName = col->getUniqueName();
        if (!isCanonicalOrStandalone(uniqueName)) {
            continue;
        }
        if (hasLowercaseID(uniqueName)) {
            continue;
        }
        appendOutputColumn(col);
    }

    // The foreign join rewrite runs before projection pushdown, so the matched
    // subtree's schema can be narrower than parent FILTER/ORDER BY requirements.
    // Keep the available graph properties in the pushed-down scan; projection
    // pushdown can prune unused columns later.
    for (auto& pattern : patterns) {
        for (auto& property : pattern->getPropertyExpressions()) {
            if (property->getPropertyName().starts_with("_")) {
                continue;
            }
            appendOutputColumn(property);
        }
    }
    // Properties referenced by pushed predicates must be evaluable above.
    for (auto& pred : filterPredicates) {
        std::vector<std::shared_ptr<Expression>> stack{pred};
        while (!stack.empty()) {
            auto expr = stack.back();
            stack.pop_back();
            if (expr->expressionType == ExpressionType::PROPERTY) {
                if (!expr->constCast<PropertyExpression>().getPropertyName().starts_with("_")) {
                    appendOutputColumn(expr);
                }
                continue;
            }
            for (auto i = 0u; i < expr->getNumChildren(); i++) {
                stack.push_back(expr->getChild(i));
            }
        }
    }

    // Fallback: preserve original scope to avoid breaking operator replacement.
    if (outputColumns.empty()) {
        for (auto& col : allColumns) {
            appendOutputColumn(col);
        }
    }
    return outputColumns;
}

// Create a new TABLE_FUNCTION_CALL with a pushed query. Result columns are
// VariableExpressions preserving the exact unique names of the logical outputs
// so upstream operators keep matching; aliases carry the display names.
static std::shared_ptr<LogicalOperator> createPushedTableFunctionCall(
    const LogicalTableFunctionCall* representativeTF, const std::string& query,
    const std::vector<std::string>& columnNames, const std::vector<std::string>& displayNames,
    const expression_vector& outputColumns) {
    auto tableFunc = representativeTF->getTableFunc();
    expression_vector resultColumns;
    for (size_t i = 0; i < outputColumns.size(); i++) {
        auto& col = outputColumns[i];
        auto dataType = col->getDataType().copy();
        std::string uniqueName = col->getUniqueName();
        auto alias = displayNames[i];
        auto variable =
            std::make_shared<VariableExpression>(std::move(dataType), uniqueName, alias);
        // Mirror the display name into the alias slot: downstream passes
        // (e.g. ORDER BY pushdown) resolve output references via getAlias().
        variable->setAlias(alias);
        resultColumns.push_back(std::move(variable));
    }
    auto originalBindData = representativeTF->getBindData();
    auto newBindData = originalBindData->copyWithQuery(query, resultColumns, columnNames);
    if (!newBindData) {
        // Extension doesn't support query modification.
        return nullptr;
    }
    // Clear column predicates since they were for single-table scans and don't
    // apply to joins. Predicates travel as FILTER operators and are folded back
    // into SQL WHERE clauses by the filter push-down pass.
    newBindData->setColumnPredicates({});
    auto tableFuncCall =
        std::make_shared<LogicalTableFunctionCall>(std::move(tableFunc), std::move(newBindData));
    tableFuncCall->computeFlatSchema();
    return tableFuncCall;
}

// Collect the unique names of column references (properties and variables) in
// an expression subtree. Literals, parameters and operator nodes carry no
// column requirement: parameters and literals stay evaluable above the pushed
// scan, and operators are re-evaluated locally.
static void collectReferencedUniqueNames(const Expression& expr,
    std::unordered_set<std::string>& out) {
    std::vector<const Expression*> stack{&expr};
    while (!stack.empty()) {
        auto node = stack.back();
        stack.pop_back();
        if (node->expressionType == ExpressionType::PROPERTY ||
            node->expressionType == ExpressionType::VARIABLE) {
            out.insert(node->getUniqueName());
        }
        for (auto i = 0u; i < node->getNumChildren(); i++) {
            stack.push_back(node->getChild(i).get());
        }
    }
}

// Top-down recursive pushdown: walk the single-child spine from the plan root
// gathering parent-required columns, and splice a WITH RECURSIVE scan in
// place of the first hash join that matches a variable-length pattern.
// Bottom-up matching cannot serve this case because the join scope always
// carries intermediate path columns that no SQL output can provide.
// Forward declaration (defined in the recursive-pushdown section below).
// ChainMatchInfo is defined above in this translation unit.
static std::shared_ptr<LogicalOperator> buildRecursiveCTEPushdown(
    ForeignJoinPushDownOptimizer* self, ChainMatchInfo& chain,
    const std::unordered_set<std::string>& needed);

bool ForeignJoinPushDownOptimizer::tryRecursiveTopDown(LogicalPlan* plan) {
    auto op = plan->getLastOperator();
    std::unordered_set<std::string> needed;
    std::vector<std::shared_ptr<LogicalOperator>> spine;
    while (true) {
        if (!op) {
            return false;
        }
        if (op->getOperatorType() == LogicalOperatorType::HASH_JOIN) {
            ChainCollector collector(context);
            if (!collector.collect(op)) {
                return false;
            }
            auto& chain = collector.chain;
            if (!chain.recursive.has_value()) {
                // Fixed-hop patterns are owned by the bottom-up pass.
                return false;
            }
            chain.outputSchema = op->getSchema();
            if (!finalizeChain(chain, context)) {
                return false;
            }
            auto pushed = buildRecursiveCTEPushdown(this, chain, needed);
            if (!pushed) {
                return false;
            }
            if (spine.empty()) {
                plan->setLastOperator(std::move(pushed));
            } else {
                spine.back()->setChild(0, std::move(pushed));
                for (auto& spineOp : spine) {
                    spineOp->computeFlatSchema();
                }
            }
            return true;
        }
        if (op->getNumChildren() != 1) {
            return false;
        }
        switch (op->getOperatorType()) {
        case LogicalOperatorType::PROJECTION: {
            for (auto& expr : op->constCast<LogicalProjection>().getExpressionsToProject()) {
                collectReferencedUniqueNames(*expr, needed);
            }
            break;
        }
        case LogicalOperatorType::FILTER: {
            collectReferencedUniqueNames(*op->constCast<LogicalFilter>().getPredicate(), needed);
            break;
        }
        case LogicalOperatorType::ORDER_BY: {
            for (auto& expr : op->constCast<LogicalOrderBy>().getExpressionsToOrderBy()) {
                collectReferencedUniqueNames(*expr, needed);
            }
            break;
        }
        case LogicalOperatorType::LIMIT:
            break;
        case LogicalOperatorType::DISTINCT: {
            for (auto& key : op->constCast<LogicalDistinct>().getKeys()) {
                collectReferencedUniqueNames(*key, needed);
            }
            break;
        }
        default:
            // AGGREGATE and everything else: keep the recursive pattern local.
            return false;
        }
        spine.push_back(op);
        op = op->getChild(0);
    }
}

std::shared_ptr<LogicalOperator> ForeignJoinPushDownOptimizer::visitOperator(
    const std::shared_ptr<LogicalOperator>& op) {
    // bottom-up traversal
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        op->setChild(i, visitOperator(op->getChild(i)));
    }
    auto result = visitOperatorReplaceSwitch(op);
    result->computeFlatSchema();
    return result;
}

std::shared_ptr<LogicalOperator> ForeignJoinPushDownOptimizer::visitHashJoinReplace(
    std::shared_ptr<LogicalOperator> op) {
    if (!op) {
        return op;
    }
    // Fast path: only hash joins can root a pushed pattern.
    if (op->getOperatorType() != LogicalOperatorType::HASH_JOIN) {
        return op;
    }
    ChainCollector collector(context);
    if (collector.collect(op)) {
        auto& chain = collector.chain;
        chain.outputSchema = op->getSchema();
        if (finalizeChain(chain, context)) {
            if (chain.recursive.has_value()) {
                // Recursive hops are matched top-down in rewrite() where the
                // parent-required columns are known; bottom-up the output
                // scope always carries intermediate path columns.
                return op;
            }
            std::unordered_map<std::string, std::string> nodeIDCols;
            for (auto& [var, node] : chain.nodeExprs) {
                auto tableIt = chain.nodeTables.find(var);
                if (tableIt == chain.nodeTables.end()) {
                    continue;
                }
                nodeIDCols[var] =
                    resolveNodeIDColumn(node.get(), tableIt->second, chain.dbRawName, context);
            }
            std::vector<std::shared_ptr<NodeOrRelExpression>> patterns;
            for (auto& hop : chain.hops) {
                patterns.push_back(hop.extend->getBoundNode());
                patterns.push_back(hop.extend->getRel());
                patterns.push_back(hop.extend->getNbrNode());
            }
            auto outputColumns =
                collectPushedOutputColumns(chain.outputSchema, patterns, chain.filterPredicates);
            auto joinQueryInfo = buildChainJoinQuery(chain, outputColumns, nodeIDCols, context);
            if (!joinQueryInfo.query.empty() &&
                joinQueryInfo.columnNames.size() == outputColumns.size()) {
                auto result =
                    createPushedTableFunctionCall(chain.representativeTF, joinQueryInfo.query,
                        joinQueryInfo.columnNames, joinQueryInfo.displayNames, outputColumns);
                if (result != nullptr) {
                    // Register the pushed scan so enclosing AGGREGATE / ORDER BY
                    // operators can fold into its SQL.
                    PushedScanInfo info;
                    info.queryTemplate = joinQueryInfo.query;
                    info.selectItems = joinQueryInfo.columnNames;
                    for (auto& [var, _] : chain.nodeTables) {
                        info.tableAliases.insert(var);
                    }
                    for (auto& hop : chain.hops) {
                        info.tableAliases.insert(hop.relVar);
                    }
                    info.idColumns = nodeIDCols;
                    auto& tfCall = result->constCast<LogicalTableFunctionCall>();
                    for (size_t i = 0; i < outputColumns.size(); i++) {
                        // Parse the SQL output alias back out of "<expr> AS <alias>".
                        auto& item = joinQueryInfo.columnNames[i];
                        auto asPos = item.rfind(" AS ");
                        auto sqlAlias = asPos == std::string::npos ?
                                            sanitizeSQLAlias(outputColumns[i]->getUniqueName()) :
                                            item.substr(asPos + 4);
                        info.outputAliases[outputColumns[i]->getUniqueName()] = sqlAlias;
                    }
                    pushedScans[tfCall.getBindData()] = std::move(info);
                    // Re-attach collected filters above the pushed scan. The
                    // filter push-down pass folds translatable ones into SQL
                    // WHERE clauses; the rest stay local (still correct).
                    for (auto& pred : chain.filterPredicates) {
                        result = std::make_shared<LogicalFilter>(pred, std::move(result));
                        result->computeFlatSchema();
                    }
                    return result;
                }
            }
        }
    }
    return op;
}

// ---------------------------------------------------------------------------
// Aggregation + ORDER BY pushdown over pushed scans.
// ---------------------------------------------------------------------------

static std::string uniqueSQLAlias(const std::string& base, std::unordered_set<std::string>& used) {
    auto alias = sanitizeSQLAlias(base);
    if (!used.contains(alias)) {
        used.insert(alias);
        return alias;
    }
    for (auto i = 1u;; i++) {
        auto candidate = std::format("{}_{}", alias, i);
        if (!used.contains(candidate)) {
            used.insert(candidate);
            return candidate;
        }
    }
}

static std::string aggregateFunctionName(const AggregateFunctionExpression& agg) {
    auto name = agg.getFunction().name;
    StringUtils::toUpper(name);
    return name;
}

// Check that an expression tree only references columns from an allowed set
// (by unique name). Used to validate keeping a projection above a rewritten
// scan whose output column set changed.
static bool expressionUsesOnlyColumns(const Expression& expr,
    const std::unordered_set<std::string>& allowed) {
    std::vector<const Expression*> stack{&expr};
    while (!stack.empty()) {
        auto node = stack.back();
        stack.pop_back();
        switch (node->expressionType) {
        case ExpressionType::PROPERTY:
        case ExpressionType::VARIABLE:
            if (!allowed.contains(node->getUniqueName())) {
                return false;
            }
            break;
        default:
            break;
        }
        for (auto i = 0u; i < node->getNumChildren(); i++) {
            stack.push_back(node->getChild(i).get());
        }
    }
    return true;
}

std::shared_ptr<planner::LogicalOperator> ForeignJoinPushDownOptimizer::visitAggregateReplace(
    std::shared_ptr<planner::LogicalOperator> op) {
    if (!op || op->getNumChildren() < 1) {
        return op;
    }
    auto& aggregate = op->constCast<LogicalAggregate>();
    // Look through a pass-through projection spine to the pushed scan. The
    // spine is kept (re-pointed at the grouped scan); anything else in
    // between keeps aggregation local.
    std::vector<std::shared_ptr<LogicalOperator>> spine;
    auto child = op->getChild(0);
    while (child->getOperatorType() == LogicalOperatorType::PROJECTION) {
        if (child->getNumChildren() < 1) {
            return op;
        }
        spine.push_back(child);
        child = child->getChild(0);
    }
    if (!isForeignTableFunctionCall(child.get())) {
        return op;
    }
    auto& tfCall = child->constCast<LogicalTableFunctionCall>();
    auto infoIt = pushedScans.find(tfCall.getBindData());
    if (infoIt == pushedScans.end() || infoIt->second.hasGroupBy) {
        // Only fold into scans built by this optimizer, and never stack two
        // GROUP BY clauses into one query template.
        return op;
    }
    auto& info = infoIt->second;
    SQLTranslationContext ctx;
    ctx.tableAliases = info.tableAliases;
    ctx.idColumns = info.idColumns;
    ctx.outputAliases = info.outputAliases;

    // Translate grouping keys.
    std::vector<std::string> groupRefs;
    expression_vector keyExprs = aggregate.getAllKeys();
    for (auto& key : keyExprs) {
        auto ref = translateOperandToSQL(*key, ctx);
        if (ref.empty()) {
            return op;
        }
        groupRefs.push_back(ref);
    }
    // Translate aggregate functions. Supported: COUNT (optionally DISTINCT, or
    // COUNT(*) with no argument), SUM, AVG, MIN, MAX over a single pushable
    // argument.
    struct AggItem {
        std::string sql;
        std::shared_ptr<Expression> expr;
    };
    std::vector<AggItem> aggItems;
    for (auto& aggExpr : aggregate.getAggregates()) {
        if (aggExpr->expressionType != ExpressionType::AGGREGATE_FUNCTION) {
            return op;
        }
        auto& agg = aggExpr->constCast<AggregateFunctionExpression>();
        auto name = aggregateFunctionName(agg);
        if (name == "COUNT_STAR") {
            name = "COUNT";
        }
        if (name != "COUNT" && name != "SUM" && name != "AVG" && name != "MIN" && name != "MAX") {
            return op;
        }
        if (agg.getNumChildren() > 1) {
            return op;
        }
        std::string arg;
        if (agg.getNumChildren() == 0) {
            if (name != "COUNT") {
                return op;
            }
            arg = "*";
        } else {
            arg = translateOperandToSQL(*agg.getChild(0), ctx);
            if (arg.empty()) {
                return op;
            }
        }
        std::string distinct;
        if (agg.isDistinct()) {
            if (name != "COUNT" || arg == "*") {
                return op;
            }
            distinct = "DISTINCT ";
        }
        aggItems.push_back({std::format("{}({}{})", name, distinct, arg), aggExpr});
    }
    if (keyExprs.empty() && aggItems.empty()) {
        return op;
    }

    // Build the new select list and result columns. SQL aliases own the
    // output-alias mapping so ORDER BY above resolves without derivation.
    std::unordered_set<std::string> usedAliases;
    std::vector<std::string> selectItems;
    expression_vector resultColumns;
    std::unordered_map<std::string, std::string> outputAliases;
    auto appendOutput = [&](const std::shared_ptr<Expression>& expr, const std::string& sqlExpr,
                            const std::string& displayBase) {
        auto alias = uniqueSQLAlias(displayBase, usedAliases);
        selectItems.push_back(std::format("{} AS {}", sqlExpr, alias));
        auto dataType = expr->getDataType().copy();
        auto variable =
            std::make_shared<VariableExpression>(std::move(dataType), expr->getUniqueName(), alias);
        variable->setAlias(alias);
        resultColumns.push_back(std::move(variable));
        outputAliases[expr->getUniqueName()] = alias;
    };
    for (size_t i = 0; i < keyExprs.size(); i++) {
        auto display = keyExprs[i]->hasAlias() ? keyExprs[i]->getAlias() : keyExprs[i]->toString();
        appendOutput(keyExprs[i], groupRefs[i], display);
    }
    for (auto& item : aggItems) {
        auto display = item.expr->hasAlias() ? item.expr->getAlias() : item.expr->getUniqueName();
        appendOutput(item.expr, item.sql, display);
    }

    std::string newTemplate = info.queryTemplate;
    if (!groupRefs.empty()) {
        newTemplate += " GROUP BY " + StringUtils::join(groupRefs, ", ");
    }

    auto newBindData = tfCall.getBindData()->copyWithQuery(newTemplate, resultColumns, selectItems);
    if (!newBindData) {
        return op;
    }
    newBindData->setColumnPredicates({});
    auto newTF =
        std::make_shared<LogicalTableFunctionCall>(tfCall.getTableFunc(), std::move(newBindData));
    newTF->computeFlatSchema();
    // The grouped scan outputs only keys + aggregates: the kept projection
    // spine must not reference anything else.
    std::unordered_set<std::string> newOutputs;
    for (auto& col : resultColumns) {
        newOutputs.insert(col->getUniqueName());
    }
    for (auto& projOp : spine) {
        auto& proj = projOp->constCast<LogicalProjection>();
        for (auto& expr : proj.getExpressionsToProject()) {
            if (!expressionUsesOnlyColumns(*expr, newOutputs)) {
                return op;
            }
        }
    }
    PushedScanInfo newInfo;
    newInfo.queryTemplate = newTemplate;
    newInfo.selectItems = selectItems;
    // Group outputs are bare select-item aliases (no table range); keep the
    // table aliases too so sibling expressions still resolve if reused.
    newInfo.tableAliases = info.tableAliases;
    newInfo.idColumns = info.idColumns;
    newInfo.outputAliases = std::move(outputAliases);
    newInfo.hasGroupBy = true;
    newInfo.hasOrderBy = info.hasOrderBy;
    pushedScans[newTF->getBindData()] = std::move(newInfo);
    if (!spine.empty()) {
        // Expand the bottom spine projection to pass through the grouped
        // outputs. Projection pushdown restarts its in-use set at every
        // PROJECTION, so without this the columns the dropped AGGREGATE used
        // to provide would be pruned from the grouped scan, dangling the
        // operators above.
        auto& bottomProj = spine.back()->constCast<LogicalProjection>();
        auto expanded = bottomProj.getExpressionsToProject();
        std::unordered_set<std::string> have;
        for (auto& expr : expanded) {
            have.insert(expr->getUniqueName());
        }
        for (auto& col : resultColumns) {
            if (have.insert(col->getUniqueName()).second) {
                expanded.push_back(col);
            }
        }
        std::shared_ptr<LogicalOperator> bottom =
            std::make_shared<LogicalProjection>(std::move(expanded), std::move(newTF));
        bottom->computeFlatSchema();
        if (spine.size() > 1) {
            spine[spine.size() - 2]->setChild(0, std::move(bottom));
            for (auto& projOp : spine) {
                projOp->computeFlatSchema();
            }
            return spine.front();
        }
        return bottom;
    }
    return newTF;
}

std::shared_ptr<planner::LogicalOperator> ForeignJoinPushDownOptimizer::visitOrderByReplace(
    std::shared_ptr<planner::LogicalOperator> op) {
    if (!op || op->getNumChildren() < 1) {
        return op;
    }
    auto& orderBy = op->constCast<LogicalOrderBy>();
    // Look through a pass-through projection spine to the pushed scan.
    // Anything else (filters, limits, ...) in between keeps the ORDER BY
    // local: baking sort keys into the template while a FILTER above could
    // later append WHERE after ORDER BY would generate invalid SQL.
    // Projections keep working unchanged: sort keys reference table columns
    // or pushed select items, never prunable outputs.
    std::vector<std::shared_ptr<LogicalOperator>> spine;
    auto child = op->getChild(0);
    while (child->getOperatorType() == LogicalOperatorType::PROJECTION) {
        if (child->getNumChildren() < 1) {
            return op;
        }
        spine.push_back(child);
        child = child->getChild(0);
    }
    if (child->getOperatorType() != LogicalOperatorType::TABLE_FUNCTION_CALL ||
        !isForeignTableFunctionCall(child.get())) {
        return op;
    }
    auto& tfCall = child->constCast<LogicalTableFunctionCall>();
    auto infoIt = pushedScans.find(tfCall.getBindData());
    if (infoIt == pushedScans.end() || infoIt->second.hasOrderBy) {
        return op;
    }
    auto& info = infoIt->second;
    SQLTranslationContext ctx;
    ctx.tableAliases = info.tableAliases;
    ctx.idColumns = info.idColumns;
    ctx.outputAliases = info.outputAliases;

    // Translate sort keys to SQL references. Table refs ("a.name") survive
    // later column pruning; output aliases reference pushed select items.
    auto orderExprs = orderBy.getExpressionsToOrderBy();
    auto ascOrders = orderBy.getIsAscOrders();

    std::vector<std::string> sortRefs;
    for (size_t i = 0; i < orderExprs.size(); i++) {
        // Any expression denoting an already-pushed select item (aggregate
        // call, WITH alias, ...) resolves to its output alias by unique name;
        // unique-name equality means value equality, so sorting by the alias
        // is exact.
        auto ref = translateOperandToSQL(*orderExprs[i], ctx);
        if (ref.empty()) {
            auto outIt = ctx.outputAliases.find(orderExprs[i]->getUniqueName());
            if (outIt != ctx.outputAliases.end()) {
                ref = outIt->second;
            }
        }
        if (ref.empty()) {
            return op;
        }
        auto asc = i < ascOrders.size() ? ascOrders[i] : true;
        sortRefs.push_back(std::format("{} {}", ref, asc ? "ASC" : "DESC"));
    }
    if (sortRefs.empty()) {
        return op;
    }
    std::string newTemplate = info.queryTemplate + " ORDER BY " + StringUtils::join(sortRefs, ", ");
    expression_vector resultColumns;
    for (auto i = 0u; i < tfCall.getBindData()->getNumColumns(); i++) {
        resultColumns.push_back(tfCall.getBindData()->columns[i]);
    }
    auto newBindData =
        tfCall.getBindData()->copyWithQuery(newTemplate, resultColumns, info.selectItems);
    if (!newBindData) {
        return op;
    }
    newBindData->setColumnPredicates({});
    auto newTF =
        std::make_shared<LogicalTableFunctionCall>(tfCall.getTableFunc(), std::move(newBindData));
    newTF->computeFlatSchema();
    PushedScanInfo newInfo = info;
    newInfo.queryTemplate = newTemplate;
    newInfo.hasOrderBy = true;
    pushedScans[newTF->getBindData()] = std::move(newInfo);
    if (!spine.empty()) {
        spine.back()->setChild(0, std::move(newTF));
        for (auto& projOp : spine) {
            projOp->computeFlatSchema();
        }
        return spine.front();
    }
    return newTF;
}

// ---------------------------------------------------------------------------
// N-hop recursive (variable-length) pushdown via WITH RECURSIVE.
// ---------------------------------------------------------------------------

// Build the WITH RECURSIVE query for a matched variable-length pattern and
// replace the whole join subtree with a single pushed scan. Every required
// output must map to an endpoint property, an endpoint ID or the path
// length; anything else (path objects, rel properties, weights) keeps the
// pattern local (fallback is returned).
// Map one required output column to a SQL reference in a recursive-CTE pushdown.
// Returns "" when the column cannot be served from endpoint bindings or the
// path length. Skippable intermediate columns (path arrays, the rel pattern
// object) are reported through skippedOut=true instead of failing: they are
// consumed inside the probe being replaced and can only appear in the scope,
// never in a parent requirement (verified against `needed` by the caller).
static std::string mapRecursiveOutputColumn(const Expression& col, const RecursiveHop& hop,
    const std::string& lengthUniqueName, const std::unordered_set<std::string>& skippable,
    bool& skippedOut, std::string& displayOut) {
    skippedOut = false;
    if (col.expressionType == ExpressionType::PROPERTY) {
        auto& prop = col.constCast<PropertyExpression>();
        auto rawVar = prop.getRawVariableName();
        auto propName = prop.getPropertyName();
        if (rawVar == hop.inputVar || rawVar == hop.outputVar) {
            auto idCol = rawVar == hop.inputVar ? hop.inputIDCol : hop.outputIDCol;
            displayOut = std::format("{}.{}", rawVar, propName);
            if (propName == InternalKeyword::ID) {
                return std::format("{}.{}", rawVar, idCol);
            }
            return std::format("{}.{}", rawVar, propName);
        }
        if (rawVar == hop.relVar && propName == InternalKeyword::LENGTH) {
            displayOut = col.hasAlias() ? col.getAlias() : "length";
            return "_p.depth";
        }
        return "";
    }
    if (col.expressionType == ExpressionType::VARIABLE) {
        if (!lengthUniqueName.empty() && col.getUniqueName() == lengthUniqueName) {
            displayOut = col.hasAlias() ? col.getAlias() : "length";
            return "_p.depth";
        }
        // Table-scan outputs absorbed into the scope ("_N_var.prop").
        auto rawVar = getExpressionRawVar(col);
        if (!rawVar.empty() && (rawVar == hop.inputVar || rawVar == hop.outputVar)) {
            auto dotPos = col.getUniqueName().find('.');
            auto propName =
                dotPos == std::string::npos ? "" : col.getUniqueName().substr(dotPos + 1);
            auto idCol = rawVar == hop.inputVar ? hop.inputIDCol : hop.outputIDCol;
            displayOut = std::format("{}.{}", rawVar, propName);
            if (propName == InternalKeyword::ID) {
                return std::format("{}.{}", rawVar, idCol);
            }
            if (!propName.empty()) {
                return std::format("{}.{}", rawVar, propName);
            }
        }
    }
    if (skippable.contains(col.getUniqueName())) {
        skippedOut = true;
        return "";
    }
    return "";
}

static std::shared_ptr<LogicalOperator> buildRecursiveCTEPushdown(
    ForeignJoinPushDownOptimizer* self, ChainMatchInfo& chain,
    const std::unordered_set<std::string>& needed) {
    auto& hop = chain.recursive.value();
    auto rx = hop.extend;
    auto& bindData = rx->getBindData();
    std::string cteName = sanitizeSQLAlias(std::format("_{}_paths", hop.relVar));
    // Anchor member: one row per edge (plus the zero-hop row when lower == 0).
    std::string anchor = std::format("SELECT {}.{} AS src, {}.{} AS dst, 1 AS depth FROM {} {}",
        hop.relVar, hop.srcEPCol, hop.relVar, hop.dstEPCol, hop.relTable, hop.relVar);
    if (!hop.edgeFilterSQL.empty()) {
        anchor += std::format(" WHERE {}", hop.edgeFilterSQL);
    }
    if (hop.lowerBound == 0) {
        anchor += std::format(" UNION ALL SELECT {}.{} AS src, {}.{} AS dst, 0 AS depth FROM {}",
            hop.inputVar, hop.inputIDCol, hop.inputVar, hop.inputIDCol, hop.inputTable);
    }
    // Recursive member: extend walked paths by one edge per iteration, capped
    // at the upper bound. The rel alias is reused (separate query scopes).
    std::string step = std::format(
        "SELECT _p.src AS src, {}.{} AS dst, _p.depth + 1 AS depth FROM {} _p JOIN {} {} ON "
        "_p.dst = {}.{} WHERE _p.depth < {}",
        hop.relVar, hop.dstEPCol, cteName, hop.relTable, hop.relVar, hop.relVar, hop.srcEPCol,
        hop.upperBound);
    if (!hop.edgeFilterSQL.empty()) {
        step += std::format(" AND {}", hop.edgeFilterSQL);
    }
    // Outer query: bind both endpoints to their node tables and apply the
    // lower bound.
    std::string outer = std::format("SELECT {{}} FROM {} {} JOIN {} _p ON {}.{} = _p.src "
                                    "JOIN {} {} ON _p.dst = {}.{}",
        hop.inputTable, hop.inputVar, cteName, hop.inputVar, hop.inputIDCol, hop.outputTable,
        hop.outputVar, hop.outputVar, hop.outputIDCol);
    if (hop.lowerBound > 1) {
        outer += std::format(" WHERE _p.depth >= {}", hop.lowerBound);
    }
    std::string query = std::format("WITH RECURSIVE {}(src, dst, depth) AS ({} UNION ALL {}) {}",
        cteName, anchor, step, outer);

    // Map required outputs. Endpoint properties/IDs resolve against the node
    // tables; the length expression resolves to the CTE depth column.
    std::string lengthUniqueName =
        bindData.lengthExpr != nullptr ? bindData.lengthExpr->getUniqueName() : "";
    std::vector<std::shared_ptr<NodeOrRelExpression>> patterns{hop.rel->getSrcNode(), hop.rel,
        hop.rel->getDstNode()};
    auto outputColumns =
        collectPushedOutputColumns(chain.outputSchema, patterns, chain.filterPredicates);
    // Intermediate columns consumed inside the replaced probe (path arrays and
    // the rel pattern object) carry no parent requirement; anything else that
    // cannot be served keeps the pattern local.
    std::unordered_set<std::string> skippable;
    if (bindData.pathNodeIDsExpr) {
        skippable.insert(bindData.pathNodeIDsExpr->getUniqueName());
    }
    if (bindData.pathEdgeIDsExpr) {
        skippable.insert(bindData.pathEdgeIDsExpr->getUniqueName());
    }
    if (bindData.directionExpr) {
        skippable.insert(bindData.directionExpr->getUniqueName());
    }
    // The rel pattern object itself: skippable unless a parent requires it
    // (enforced by the needed-coverage check below).
    skippable.insert(hop.rel->getUniqueName());
    std::vector<std::string> columnNames;
    std::vector<std::string> displayNames;
    std::unordered_set<std::string> usedAliases;
    std::unordered_set<std::string> emitted;
    for (auto& col : outputColumns) {
        bool skipped = false;
        std::string display;
        auto ref =
            mapRecursiveOutputColumn(*col, hop, lengthUniqueName, skippable, skipped, display);
        if (ref.empty()) {
            if (!skipped) {
                return nullptr;
            }
            continue;
        }
        auto alias = uniqueSQLAlias(display, usedAliases);
        columnNames.push_back(std::format("{} AS {}", ref, alias));
        displayNames.push_back(display);
        emitted.insert(col->getUniqueName());
    }
    for (auto& name : needed) {
        if (!emitted.contains(name)) {
            return nullptr;
        }
    }
    if (columnNames.size() != displayNames.size()) {
        return nullptr;
    }
    // Rebuild the output column list without skipped intermediates so the
    // result schema matches the select list exactly.
    expression_vector pushedOutputs;
    for (auto& col : outputColumns) {
        if (emitted.contains(col->getUniqueName())) {
            pushedOutputs.push_back(col);
        }
    }
    auto result = createPushedTableFunctionCall(chain.representativeTF, query, columnNames,
        displayNames, pushedOutputs);
    if (result == nullptr) {
        return nullptr;
    }
    ForeignJoinPushDownOptimizer::PushedScanInfo pushed;
    pushed.queryTemplate = query;
    pushed.selectItems = columnNames;
    pushed.tableAliases.insert(hop.inputVar);
    pushed.tableAliases.insert(hop.outputVar);
    pushed.tableAliases.insert(hop.relVar);
    pushed.idColumns[hop.inputVar] = hop.inputIDCol;
    pushed.idColumns[hop.outputVar] = hop.outputIDCol;
    auto& tfCall = result->constCast<LogicalTableFunctionCall>();
    for (size_t i = 0; i < pushedOutputs.size(); i++) {
        auto& item = columnNames[i];
        auto asPos = item.rfind(" AS ");
        pushed.outputAliases[pushedOutputs[i]->getUniqueName()] =
            asPos == std::string::npos ? item : item.substr(asPos + 4);
    }
    self->registerPushedScan(tfCall.getBindData(), std::move(pushed));
    for (auto& pred : chain.filterPredicates) {
        result = std::make_shared<LogicalFilter>(pred, std::move(result));
        result->computeFlatSchema();
    }
    return result;
}

} // namespace optimizer
} // namespace lbug

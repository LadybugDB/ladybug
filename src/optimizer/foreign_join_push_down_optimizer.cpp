#include "optimizer/foreign_join_push_down_optimizer.h"

#include <algorithm>
#include <cctype>

#include "binder/expression/property_expression.h"
#include "binder/expression/variable_expression.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/string_utils.h"
#include "main/database_manager.h"
#include "planner/operator/extend/logical_extend.h"
#include "planner/operator/logical_filter.h"
#include "planner/operator/logical_flatten.h"
#include "planner/operator/logical_hash_join.h"
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
    visitOperator(plan->getLastOperator());
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
    // three-way equality check below works.
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

// Structure to hold extracted pattern info
struct ForeignJoinPatternInfo {
    // The extend operator
    const LogicalExtend* extend = nullptr;
    // Table function calls for node scans
    const LogicalTableFunctionCall* srcTableFunc = nullptr;
    const LogicalTableFunctionCall* dstTableFunc = nullptr;
    // Intermediate operators
    const LogicalHashJoin* outerHashJoin = nullptr;
    const LogicalHashJoin* innerHashJoin = nullptr;
    const LogicalFilter* relFilter = nullptr;
    // Original output schema
    const Schema* outputSchema = nullptr;
    // Table names extracted from bind data
    std::string srcTable;
    std::string dstTable;
    std::string relTable;
    std::string dbName; // Foreign database name
};

// Try to match the foreign join pattern and extract info
static std::optional<ForeignJoinPatternInfo> matchPattern(const LogicalOperator* op,
    main::ClientContext* context) {
    if (op == nullptr) {
        return std::nullopt;
    }

    ForeignJoinPatternInfo info;
    info.outputSchema = op->getSchema();

    // Check if we have HASH_JOIN at top
    if (op->getOperatorType() != LogicalOperatorType::HASH_JOIN) {
        return std::nullopt;
    }

    if (op->getNumChildren() < 2) {
        return std::nullopt;
    }

    info.outerHashJoin = op->constPtrCast<LogicalHashJoin>();
    if (info.outerHashJoin->getJoinType() != JoinType::INNER) {
        return std::nullopt;
    }

    // Check build side is TABLE_FUNCTION_CALL (destination node's scan)
    auto buildChild = op->getChild(1).get();
    if (buildChild == nullptr || !isForeignTableFunctionCall(buildChild)) {
        return std::nullopt;
    }
    info.dstTableFunc = buildChild->constPtrCast<LogicalTableFunctionCall>();

    // Check probe side - can be FLATTEN or direct HASH_JOIN
    auto probeOp = op->getChild(0).get();
    if (probeOp == nullptr) {
        return std::nullopt;
    }
    if (probeOp->getOperatorType() == LogicalOperatorType::FLATTEN) {
        if (probeOp->getNumChildren() < 1) {
            return std::nullopt;
        }
        probeOp = probeOp->getChild(0).get();
        if (probeOp == nullptr) {
            return std::nullopt;
        }
    }

    // Now probeOp should be HASH_JOIN
    if (probeOp->getOperatorType() != LogicalOperatorType::HASH_JOIN) {
        return std::nullopt;
    }

    if (probeOp->getNumChildren() < 2) {
        return std::nullopt;
    }

    info.innerHashJoin = probeOp->constPtrCast<LogicalHashJoin>();
    if (info.innerHashJoin->getJoinType() != JoinType::INNER) {
        return std::nullopt;
    }

    // Inner hash join build side should be TABLE_FUNCTION_CALL (source node's scan)
    auto innerBuildChild = probeOp->getChild(1).get();
    if (innerBuildChild == nullptr || !isForeignTableFunctionCall(innerBuildChild)) {
        return std::nullopt;
    }
    info.srcTableFunc = innerBuildChild->constPtrCast<LogicalTableFunctionCall>();

    // Inner hash join probe side should be EXTEND
    auto extendOp = probeOp->getChild(0).get();
    if (extendOp != nullptr && extendOp->getOperatorType() == LogicalOperatorType::FILTER) {
        info.relFilter = extendOp->constPtrCast<LogicalFilter>();
        if (extendOp->getNumChildren() < 1) {
            return std::nullopt;
        }
        extendOp = extendOp->getChild(0).get();
    }
    if (extendOp == nullptr || extendOp->getOperatorType() != LogicalOperatorType::EXTEND) {
        return std::nullopt;
    }

    info.extend = extendOp->constPtrCast<LogicalExtend>();

    // The extend's child should be SCAN_NODE_TABLE
    if (extendOp->getNumChildren() < 1 || extendOp->getChild(0) == nullptr ||
        extendOp->getChild(0)->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE) {
        return std::nullopt;
    }

    // Check that the rel entry has a foreign scan function
    if (!hasForeignScanFunction(info.extend->getRel().get())) {
        return std::nullopt;
    }

    // Verify all are from the same foreign database
    auto srcDbName = getNodeForeignDatabaseName(info.extend->getBoundNode().get(), context);
    auto dstDbName = getNodeForeignDatabaseName(info.extend->getNbrNode().get(), context);
    auto relDbName = getRelForeignDatabaseName(info.extend->getRel().get(), context);

    if (srcDbName.empty() || dstDbName.empty() || relDbName.empty()) {
        return std::nullopt;
    }
    if (srcDbName != dstDbName || srcDbName != relDbName) {
        return std::nullopt;
    }

    // Extract just the database name (without type suffix like "(DUCKDB)")
    if (srcDbName.empty()) {
        return std::nullopt;
    }
    auto parenPos = srcDbName.find('(');
    if (parenPos != std::string::npos) {
        info.dbName = srcDbName.substr(0, parenPos);
    } else {
        info.dbName = srcDbName;
    }

    // Extract table names from bind data descriptions
    auto extractTableName = [](const std::string& desc) -> std::string {
        auto fromPos = desc.find("FROM ");
        if (fromPos == std::string::npos) {
            return "";
        }
        auto tableName = desc.substr(fromPos + 5);
        // Remove any trailing clauses (WHERE, LIMIT, etc.)
        auto spacePos = tableName.find(' ');
        if (spacePos != std::string::npos) {
            tableName = tableName.substr(0, spacePos);
        }
        return tableName;
    };

    auto srcDesc = info.srcTableFunc->getBindData()->getDescription();
    auto dstDesc = info.dstTableFunc->getBindData()->getDescription();
    info.srcTable = extractTableName(srcDesc);
    info.dstTable = extractTableName(dstDesc);

    if (info.srcTable.empty() || info.dstTable.empty()) {
        return std::nullopt;
    }

    // Get rel table from storage
    auto rel = info.extend->getRel();
    auto relEntry = rel->getEntry(0)->ptrCast<RelGroupCatalogEntry>();
    std::string relStorage = relEntry->getStorage();

    // Parse storage format "db.table" to get full table reference
    auto dotPos = relStorage.find('.');
    if (dotPos != std::string::npos) {
        // Format: "dbname.tablename" -> need to construct proper SQL table reference
        // The source table gives us the pattern to follow
        auto srcDotPos = info.srcTable.find('.');
        if (srcDotPos != std::string::npos) {
            // Copy the database/schema part from src and append rel table name
            auto dbSchema = info.srcTable.substr(0, info.srcTable.rfind('.') + 1);
            info.relTable = dbSchema + relStorage.substr(dotPos + 1);
        } else {
            info.relTable = relStorage.substr(dotPos + 1);
        }
    } else {
        info.relTable = relStorage;
    }

    if (info.relTable.empty()) {
        return std::nullopt;
    }

    return info;
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

struct JoinQueryInfo {
    std::string query;
    std::vector<std::string> columnNames;
    std::vector<std::string> displayNames;
};

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

// Build the SQL join query string and collect column names for result mapping.
// Keep result column names SQL-safe while using display names that preserve the user's labels.
static JoinQueryInfo buildJoinQuery(const ForeignJoinPatternInfo& info,
    const expression_vector& outputColumns, main::ClientContext* context) {
    if (!info.extend || !context) {
        return {};
    }
    auto extend = info.extend;
    auto srcNode = extend->getBoundNode();
    auto dstNode = extend->getNbrNode();
    auto rel = extend->getRel();
    if (!srcNode || !dstNode || !rel) {
        return {};
    }
    // Get raw variable names (user-facing, like 'a', 'b', 'c')
    std::string srcAlias = srcNode->getVariableName();
    std::string dstAlias = dstNode->getVariableName();
    std::string relAlias = rel->getVariableName();

    // Determine join columns based on direction and foreign table schema.
    // Prefer endpoint columns identified by the src/dst naming convention
    // (SQL catalogs such as Iceberg or Unity Catalog do not guarantee that
    // the endpoint columns come first); fall back to the first two columns.
    std::string srcJoinCol, dstJoinCol;
    auto tableColumnNames = getForeignTableColumnNames(info.dbName, info.relTable, context);
    if (tableColumnNames.size() < 2) {
        throw RuntimeException(std::format(
            "Foreign join push down optimizer: unable to retrieve column names for table '{}.{}', "
            "got {} columns but need at least 2 for join",
            info.dbName, info.relTable, tableColumnNames.size()));
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
    // All-or-nothing: a lone prefix match without its counterpart is ambiguous
    // (it may be a non-endpoint property that happens to share the prefix), so
    // fall back to ordinal position for both columns to preserve the prior
    // "first two columns" invariant.
    if (!srcCol.empty() && !dstCol.empty()) {
        firstCol = srcCol;
        secondCol = dstCol;
    }
    if (extend->getDirection() == ExtendDirection::FWD) {
        srcJoinCol = firstCol;
        dstJoinCol = secondCol;
    } else {
        srcJoinCol = secondCol;
        dstJoinCol = firstCol;
    }

    // Resolve the node-table ID column without assuming column order. The
    // bound catalog entry's primary key is the single source of truth (the
    // extension registers foreign column names as property names, so it names
    // the foreign column directly); only fall back to a foreign lookup when
    // the entry carries no primary key (e.g. FOREIGN_TABLE_ENTRY).
    auto getNodeIDColumn = [&](const NodeExpression* node, const std::string& tableName) {
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
        auto columnNames = getForeignTableColumnNames(info.dbName, tableName, context);
        if (columnNames.empty()) {
            return std::string{InternalKeyword::ID};
        }
        // SQL catalogs do not guarantee the PK column comes first; prefer a
        // column literally named `id` over ordinal position.
        for (auto& column : columnNames) {
            auto lowerCol = column;
            common::StringUtils::toLower(lowerCol);
            if (lowerCol == "id") {
                return column;
            }
        }
        return columnNames[0];
    };
    auto srcIDCol = getNodeIDColumn(srcNode.get(), info.srcTable);
    auto dstIDCol = getNodeIDColumn(dstNode.get(), info.dstTable);

    // Build SELECT items from output columns and collect column names
    std::vector<std::string> columnNames;
    std::vector<std::string> displayNames;

    for (auto& col : outputColumns) {
        std::string colExpr;
        std::string colName;
        std::string displayName;

        // Determine which table the column comes from based on variable name
        if (col->expressionType == ExpressionType::PROPERTY) {
            auto& prop = col->constCast<PropertyExpression>();
            // Use raw variable name for SQL query (e.g., 'a' instead of '_0_a')
            auto rawVarName = prop.getRawVariableName();
            auto propName = prop.getPropertyName();

            if (propName == InternalKeyword::ID) {
                auto idColumn = rawVarName == srcAlias ? srcIDCol : dstIDCol;
                colExpr = std::format("{}.{}", rawVarName, idColumn);
            } else {
                colExpr = std::format("{}.{}", rawVarName, propName);
            }
            colName = sanitizeSQLAlias(std::format("{}_{}", rawVarName, propName));
            displayName = std::format("{}.{}", rawVarName, propName);
        } else {
            // For non-property expressions, parse the unique name to extract table alias and column
            auto uniqueName = col->getUniqueName();

            // Parse format: "_N_varname.columnname" -> "varname.columnname".
            auto dotPos = uniqueName.find('.');
            if (dotPos != std::string::npos) {
                auto prefix = uniqueName.substr(0, dotPos);       // "_N_varname"
                auto colNamePart = uniqueName.substr(dotPos + 1); // "columnname"

                // Extract raw variable name by removing the "_N_" prefix
                // Format is typically "_0_a", "_2_c", etc.
                auto underscorePos = prefix.find('_', 1); // Find second underscore
                if (underscorePos != std::string::npos) {
                    auto rawVar = prefix.substr(underscorePos + 1); // "a", "c", etc.
                    colExpr = std::format("{}.{}", rawVar, colNamePart);
                    colName = sanitizeSQLAlias(std::format("{}_{}", rawVar, colNamePart));
                    displayName = std::format("{}.{}", rawVar, colNamePart);
                } else {
                    // Fallback: use the whole prefix
                    colExpr = std::format("{}.{}", prefix, colNamePart);
                    colName = sanitizeSQLAlias(uniqueName);
                    displayName = uniqueName;
                }
            } else {
                // No dot, use as-is
                colExpr = uniqueName;
                colName = sanitizeSQLAlias(uniqueName);
                displayName = uniqueName;
            }
        }

        columnNames.push_back(std::format("{} AS {}", colExpr, colName));
        displayNames.push_back(displayName);
    }

    // Build the full query with proper JOIN syntax
    // Join on each node table's external ID column and the relationship table endpoint columns.
    std::string query = std::format("SELECT {{}} FROM {} {} "
                                    "JOIN {} {} ON {}.{} = {}.{} "
                                    "JOIN {} {} ON {}.{} = {}.{}",
        info.srcTable, srcAlias, info.relTable, relAlias, srcAlias, srcIDCol, relAlias, srcJoinCol,
        info.dstTable, dstAlias, relAlias, dstJoinCol, dstAlias, dstIDCol);

    return {std::move(query), std::move(columnNames), std::move(displayNames)};
}

// Create a new TABLE_FUNCTION_CALL with the join query
static std::shared_ptr<LogicalOperator> createJoinTableFunctionCall(
    const ForeignJoinPatternInfo& info, const std::string& joinQuery,
    const std::vector<std::string>& columnNames, const std::vector<std::string>& displayNames,
    const expression_vector& outputColumns) {
    // Copy the table function from the source node's scan
    auto tableFunc = info.srcTableFunc->getTableFunc();

    // Create VariableExpressions for the result columns
    // The table function expects VariableExpressions, not PropertyExpressions
    expression_vector resultColumns;
    for (size_t i = 0; i < outputColumns.size(); i++) {
        auto& col = outputColumns[i];
        auto dataType = col->getDataType().copy();

        // Preserve the exact unique name from the original bound expression so
        // upstream projections can match and replace PropertyExpressions.
        std::string uniqueName = col->getUniqueName();

        auto alias = displayNames[i];

        resultColumns.push_back(
            std::make_shared<VariableExpression>(std::move(dataType), uniqueName, alias));
    }

    // Create new bind data with the join query using the extension's copyWithQuery
    auto originalBindData = info.srcTableFunc->getBindData();
    auto newBindData = originalBindData->copyWithQuery(joinQuery, resultColumns, columnNames);

    if (!newBindData) {
        // Extension doesn't support query modification, return nullptr to indicate failure
        return nullptr;
    }

    // Clear column predicates since they were for single-table scans and don't apply to joins
    // The join conditions are already built into the query
    newBindData->setColumnPredicates({});

    auto tableFuncCall =
        std::make_shared<LogicalTableFunctionCall>(std::move(tableFunc), std::move(newBindData));

    tableFuncCall->computeFlatSchema();
    return tableFuncCall;
}

std::shared_ptr<LogicalOperator> ForeignJoinPushDownOptimizer::visitHashJoinReplace(
    std::shared_ptr<LogicalOperator> op) {
    if (!op) {
        return op;
    }
    auto patternInfo = matchPattern(op.get(), this->context);
    if (!patternInfo.has_value()) {
        return op;
    }

    auto& info = patternInfo.value();

    // Build the SQL join query and get column names.
    // Prefer canonical variable expressions while dropping helper columns like
    // "a.id" when a canonical pattern-variable expression "_N_a.id" exists.
    auto allColumns = info.outputSchema->getExpressionsInScope();
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
    // hash join's schema can be narrower than parent FILTER/ORDER BY/PROJECTION
    // requirements. Keep the available graph properties in the pushed-down scan;
    // projection pushdown can prune unused columns later.
    auto appendPatternProperties = [&](const std::shared_ptr<NodeOrRelExpression>& pattern) {
        for (auto& property : pattern->getPropertyExpressions()) {
            if (property->getPropertyName().starts_with("_")) {
                continue;
            }
            appendOutputColumn(property);
        }
    };
    appendPatternProperties(info.extend->getBoundNode());
    appendPatternProperties(info.extend->getRel());
    appendPatternProperties(info.extend->getNbrNode());

    // Fallback: if no property/variable columns were identified, preserve
    // original scope to avoid breaking operator replacement.
    if (outputColumns.empty()) {
        for (auto& col : allColumns) {
            appendOutputColumn(col);
        }
    }

    auto joinQueryInfo = buildJoinQuery(info, outputColumns, this->context);

    // Create the optimized table function call
    if (joinQueryInfo.query.empty()) {
        // buildJoinQuery bailed out (missing context, null nodes, etc.) — don't
        // attempt to push down a malformed query, just keep the original op.
        return op;
    }
    auto result = createJoinTableFunctionCall(info, joinQueryInfo.query, joinQueryInfo.columnNames,
        joinQueryInfo.displayNames, outputColumns);
    if (!result) {
        // Extension doesn't support query modification, return original
        return op;
    }

    if (info.relFilter != nullptr) {
        result = std::make_shared<LogicalFilter>(info.relFilter->getPredicate(), std::move(result));
        result->computeFlatSchema();
    }
    return result;
}

} // namespace optimizer
} // namespace lbug

#include "optimizer/foreign_join_push_down_optimizer.h"

#include <algorithm>
#include <cctype>

#include "binder/expression/property_expression.h"
#include "binder/expression/variable_expression.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/runtime.h"
#include "main/database_manager.h"
#include "planner/operator/extend/logical_extend.h"
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
    // First try the stored foreignDatabaseName
    auto storedName = relEntry->getForeignDatabaseName();
    if (!storedName.empty()) {
        return storedName;
    }
    // For foreign rel tables, extract from storage
    auto storage = relEntry->getStorage();
    auto dotPos = storage.find('.');
    if (dotPos == std::string::npos) {
        return "";
    }
    auto dbName = storage.substr(0, dotPos);
    auto dbManager = main::DatabaseManager::Get(*context);
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
    // Original output schema
    const Schema* outputSchema = nullptr;
    // Table names extracted from bind data
    std::string srcTable;
    std::string dstTable;
    std::string relTable;
    std::string dbName; // Foreign database name
};

static std::string getUnqualifiedTableName(std::string tableName) {
    auto dotPos = tableName.rfind('.');
    if (dotPos != std::string::npos) {
        tableName = tableName.substr(dotPos + 1);
    }
    if (tableName.size() >= 2 && tableName.front() == '"' && tableName.back() == '"') {
        tableName = tableName.substr(1, tableName.size() - 2);
    }
    return tableName;
}

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

// Helper to get column names from a foreign table
static std::vector<std::string> getForeignTableColumnNames(const std::string& dbName,
    const std::string& tableName, main::ClientContext* context) {
    auto dbManager = main::DatabaseManager::Get(*context);
    auto attachedDB = dbManager->getAttachedDatabase(dbName);
    if (!attachedDB) {
        return {};
    }
    return attachedDB->getTableColumnNames(tableName);
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
    auto extend = info.extend;
    auto srcNode = extend->getBoundNode();
    auto dstNode = extend->getNbrNode();
    auto rel = extend->getRel();

    // Get raw variable names (user-facing, like 'a', 'b', 'c')
    std::string srcAlias = srcNode->getVariableName();
    std::string dstAlias = dstNode->getVariableName();
    std::string relAlias = rel->getVariableName();

    // Determine join columns based on direction and foreign table schema
    std::string srcJoinCol, dstJoinCol;
    auto tableColumnNames =
        getForeignTableColumnNames(info.dbName, getUnqualifiedTableName(info.relTable), context);
    if (tableColumnNames.size() < 2) {
        throw RuntimeException(std::format(
            "Foreign join push down optimizer: unable to retrieve column names for table '{}.{}', "
            "got {} columns but need at least 2 for join",
            info.dbName, info.relTable, tableColumnNames.size()));
    }

    std::string firstCol = tableColumnNames[0];
    std::string secondCol = tableColumnNames[1];
    if (extend->getDirection() == ExtendDirection::FWD) {
        srcJoinCol = firstCol;
        dstJoinCol = secondCol;
    } else {
        srcJoinCol = secondCol;
        dstJoinCol = firstCol;
    }

    auto getNodeIDColumn = [&](const std::string& tableName) {
        auto columnNames =
            getForeignTableColumnNames(info.dbName, getUnqualifiedTableName(tableName), context);
        if (columnNames.empty()) {
            return std::string{InternalKeyword::ID};
        }
        return columnNames[0];
    };
    auto srcIDCol = getNodeIDColumn(info.srcTable);
    auto dstIDCol = getNodeIDColumn(info.dstTable);

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
    std::unordered_set<std::string> canonicalVarProps;

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
        outputColumns.push_back(col);
    }

    // Fallback: if no property/variable columns were identified, preserve
    // original scope to avoid breaking operator replacement.
    if (outputColumns.empty()) {
        for (auto& col : allColumns) {
            outputColumns.push_back(col);
        }
    }

    auto joinQueryInfo = buildJoinQuery(info, outputColumns, this->context);

    // Create the optimized table function call
    auto result = createJoinTableFunctionCall(info, joinQueryInfo.query, joinQueryInfo.columnNames,
        joinQueryInfo.displayNames, outputColumns);
    if (!result) {
        // Extension doesn't support query modification, return original
        return op;
    }

    return result;
}

} // namespace optimizer
} // namespace lbug

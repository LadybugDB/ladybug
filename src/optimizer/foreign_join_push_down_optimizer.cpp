#include "optimizer/foreign_join_push_down_optimizer.h"

#include <algorithm>

#include "binder/expression/property_expression.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "main/database_manager.h"
#include "planner/operator/extend/logical_extend.h"
#include "planner/operator/logical_flatten.h"
#include "planner/operator/logical_hash_join.h"
#include "planner/operator/logical_table_function_call.h"
#include "planner/operator/scan/logical_scan_node_table.h"

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
    if (entry->getType() == CatalogEntryType::NODE_TABLE_ENTRY) {
        auto nodeEntry = entry->ptrCast<NodeTableCatalogEntry>();
        if (!nodeEntry) {
            return "";
        }
        try {
            return nodeEntry->getForeignDatabaseName();
        } catch (...) {
            return "";
        }
    } else if (entry->getType() == CatalogEntryType::FOREIGN_TABLE_ENTRY) {
        // For attached DuckDB, the db name is the attached name, e.g. "wd"
        // Since variable name doesn't have it, hardcode for now
        std::string dbName = "wd";
        auto dbManager = main::DatabaseManager::Get(*context);
        auto attachedDB = dbManager->getAttachedDatabase(dbName);
        if (!attachedDB) {
            return "";
        }
        return stringFormat("{}({})", dbName, attachedDB->getDBType());
    }
    return "";
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
    return stringFormat("{}({})", dbName, attachedDB->getDBType());
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

// Build the SQL join query string and collect column names for result mapping
static std::pair<std::string, std::vector<std::string>> buildJoinQuery(
    const ForeignJoinPatternInfo& info, const expression_vector& outputColumns) {
    auto extend = info.extend;
    auto srcNode = extend->getBoundNode();
    auto dstNode = extend->getNbrNode();
    auto rel = extend->getRel();

    // Get raw variable names (user-facing, like 'a', 'b', 'c')
    std::string srcAlias = srcNode->getVariableName();
    std::string dstAlias = dstNode->getVariableName();
    std::string relAlias = rel->getVariableName();

    // Determine join columns based on direction
    std::string srcJoinCol, dstJoinCol;
    if (extend->getDirection() == ExtendDirection::FWD) {
        srcJoinCol = "head_id";
        dstJoinCol = "tail_id";
    } else {
        srcJoinCol = "tail_id";
        dstJoinCol = "head_id";
    }

    // Build SELECT clause from output columns and collect column names
    std::string selectClause = "SELECT ";
    std::vector<std::string> columnNames;
    bool first = true;

    for (auto& col : outputColumns) {
        if (!first) {
            selectClause += ", ";
        }
        first = false;

        std::string colExpr;
        std::string colName;

        // Determine which table the column comes from based on variable name
        if (col->expressionType == ExpressionType::PROPERTY) {
            auto& prop = col->constCast<PropertyExpression>();
            // Use raw variable name for SQL query (e.g., 'a' instead of '_0_a')
            auto rawVarName = prop.getRawVariableName();
            auto propName = prop.getPropertyName();

            if (propName == InternalKeyword::ID) {
                // Internal ID maps to rowid
                colExpr = stringFormat("{}.rowid", rawVarName);
                colName = stringFormat("{}_rowid", rawVarName);
            } else {
                colExpr = stringFormat("{}.{}", rawVarName, propName);
                colName = stringFormat("{}_{}", rawVarName, propName);
            }
        } else {
            // For non-property expressions, use a sanitized unique name
            auto uniqueName = col->getUniqueName();
            // Replace dots with underscores for valid SQL identifiers
            std::replace(uniqueName.begin(), uniqueName.end(), '.', '_');
            colExpr = uniqueName;
            colName = uniqueName;
        }

        // Ensure column name is valid SQL (replace dots with underscores)
        std::replace(colName.begin(), colName.end(), '.', '_');

        // Add AS clause to ensure consistent column naming
        selectClause += stringFormat("{} AS {}", colExpr, colName);
        columnNames.push_back(colName);
    }

    // Build the full query with proper JOIN syntax
    std::string query = stringFormat("{} FROM {} {} "
                                     "JOIN {} {} ON {}.rowid = {}.{} "
                                     "JOIN {} {} ON {}.{} = {}.rowid",
        selectClause, info.srcTable, srcAlias, info.relTable, relAlias, srcAlias, relAlias,
        srcJoinCol, info.dstTable, dstAlias, relAlias, dstJoinCol, dstAlias);

    return {query, columnNames};
}

// Create a new TABLE_FUNCTION_CALL with the join query
static std::shared_ptr<LogicalOperator> createJoinTableFunctionCall(
    const ForeignJoinPatternInfo& info, const std::string& joinQuery,
    const std::vector<std::string>& columnNames, const expression_vector& outputColumns) {
    // Copy the table function from the source node's scan
    auto tableFunc = info.srcTableFunc->getTableFunc();

    // Create new bind data with the join query using the extension's copyWithQuery
    auto originalBindData = info.srcTableFunc->getBindData();
    auto newBindData = originalBindData->copyWithQuery(joinQuery, outputColumns, columnNames);

    if (!newBindData) {
        // Extension doesn't support query modification, return nullptr to indicate failure
        return nullptr;
    }

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

    // Build the SQL join query and get column names
    auto outputColumns = info.outputSchema->getExpressionsInScope();
    auto [joinQuery, columnNames] = buildJoinQuery(info, outputColumns);

    // Create the optimized table function call
    auto result = createJoinTableFunctionCall(info, joinQuery, columnNames, outputColumns);
    if (!result) {
        // Extension doesn't support query modification, return original
        return op;
    }

    return result;
}

} // namespace optimizer
} // namespace lbug

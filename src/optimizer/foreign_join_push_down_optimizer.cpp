#include "optimizer/foreign_join_push_down_optimizer.h"

#include "binder/expression/property_expression.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
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
static std::string getNodeForeignDatabaseName(const NodeExpression* node) {
    if (node->getNumEntries() != 1) {
        return "";
    }
    auto entry = node->getEntry(0);
    auto nodeEntry = entry->ptrCast<NodeTableCatalogEntry>();
    return nodeEntry ? nodeEntry->getForeignDatabaseName() : "";
}

// Helper to get foreign database name from a rel group entry
static std::string getRelForeignDatabaseName(const RelExpression* rel) {
    if (rel->getNumEntries() != 1) {
        return "";
    }
    auto relEntry = rel->getEntry(0)->ptrCast<RelGroupCatalogEntry>();
    return relEntry ? relEntry->getForeignDatabaseName() : "";
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
};

// Try to match the foreign join pattern and extract info
static std::optional<ForeignJoinPatternInfo> matchPattern(const LogicalOperator* op) {
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
    auto srcDbName = getNodeForeignDatabaseName(info.extend->getBoundNode().get());
    auto dstDbName = getNodeForeignDatabaseName(info.extend->getNbrNode().get());
    auto relDbName = getRelForeignDatabaseName(info.extend->getRel().get());

    if (srcDbName.empty() || dstDbName.empty() || relDbName.empty()) {
        return std::nullopt;
    }
    if (srcDbName != dstDbName || srcDbName != relDbName) {
        return std::nullopt;
    }

    return info;
}

// Build the SQL join query string
static std::string buildJoinQuery(const ForeignJoinPatternInfo& info,
    const expression_vector& outputColumns) {
    auto extend = info.extend;
    auto srcNode = extend->getBoundNode();
    auto dstNode = extend->getNbrNode();
    auto rel = extend->getRel();

    // Get aliases
    std::string srcAlias = srcNode->getVariableName();
    std::string dstAlias = dstNode->getVariableName();
    std::string relAlias = rel->getVariableName();

    // Get table names from bind data descriptions
    // The description contains the SQL query, we need to extract table names
    auto srcDesc = info.srcTableFunc->getBindData()->getDescription();
    auto dstDesc = info.dstTableFunc->getBindData()->getDescription();

    // Extract table name from "SELECT {} FROM tablename" pattern
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

    std::string srcTable = extractTableName(srcDesc);
    std::string dstTable = extractTableName(dstDesc);

    // Get rel table from storage
    auto relEntry = rel->getEntry(0)->ptrCast<RelGroupCatalogEntry>();
    std::string relStorage = relEntry->getStorage();

    // Parse storage format "db.table" to get full table reference
    std::string relTable;
    auto dotPos = relStorage.find('.');
    if (dotPos != std::string::npos) {
        // Format: "dbname.tablename" -> need to construct proper SQL table reference
        // The source table gives us the pattern to follow
        auto srcDotPos = srcTable.find('.');
        if (srcDotPos != std::string::npos) {
            // Copy the database/schema part from src and append rel table name
            auto dbSchema = srcTable.substr(0, srcTable.rfind('.') + 1);
            relTable = dbSchema + relStorage.substr(dotPos + 1);
        } else {
            relTable = relStorage.substr(dotPos + 1);
        }
    } else {
        relTable = relStorage;
    }

    // Determine join columns based on direction
    std::string srcJoinCol, dstJoinCol;
    if (extend->getDirection() == ExtendDirection::FWD) {
        srcJoinCol = "head_id";
        dstJoinCol = "tail_id";
    } else {
        srcJoinCol = "tail_id";
        dstJoinCol = "head_id";
    }

    // Build SELECT clause from output columns
    std::string selectClause = "SELECT ";
    bool first = true;

    for (auto& col : outputColumns) {
        if (!first) {
            selectClause += ", ";
        }
        first = false;

        // Determine which table the column comes from based on variable name
        if (col->expressionType == ExpressionType::PROPERTY) {
            auto& prop = col->constCast<PropertyExpression>();
            auto varName = prop.getVariableName();
            auto propName = prop.getPropertyName();

            if (propName == InternalKeyword::ID) {
                // Internal ID maps to rowid
                selectClause += stringFormat("{}.rowid", varName);
            } else {
                selectClause += stringFormat("{}.{}", varName, propName);
            }
        } else {
            // For non-property expressions, use the unique name
            selectClause += col->getUniqueName();
        }
    }

    // Build the full query
    std::string query = stringFormat(
        "{} FROM {} {} "
        "JOIN {} {} ON {}.rowid = {}.{} "
        "JOIN {} {} ON {}.{} = {}.rowid",
        selectClause, srcTable, srcAlias, relTable, relAlias, srcAlias, relAlias, srcJoinCol,
        dstTable, dstAlias, relAlias, dstJoinCol, dstAlias);

    return query;
}

// Create a new TABLE_FUNCTION_CALL with the join query
static std::shared_ptr<LogicalOperator> createJoinTableFunctionCall(
    const ForeignJoinPatternInfo& info, const std::string& joinQuery) {
    // Copy the table function from the source node's scan
    auto tableFunc = info.srcTableFunc->getTableFunc();

    // Create new bind data with the join query
    // We need to clone the original bind data and update its query
    auto originalBindData = info.srcTableFunc->getBindData();
    auto newBindData = originalBindData->copy();

    // Get output columns from the schema
    auto outputColumns = info.outputSchema->getExpressionsInScope();
    newBindData->columns = outputColumns;

    // The new bind data needs column names for the result converter
    // For now, we set the columns and let the description show the join query
    // TODO: Set proper column skip flags based on what's actually needed

    auto tableFuncCall =
        std::make_shared<LogicalTableFunctionCall>(std::move(tableFunc), std::move(newBindData));

    // The join query will be used as description/printed info
    // The actual query execution will use the columns from the original bind data
    // This is a simplified approach - a full implementation would need to properly
    // construct bind data that generates the join query

    tableFuncCall->computeFlatSchema();
    return tableFuncCall;
}

std::shared_ptr<LogicalOperator> ForeignJoinPushDownOptimizer::visitHashJoinReplace(
    std::shared_ptr<LogicalOperator> op) {
    auto patternInfo = matchPattern(op.get());
    if (!patternInfo.has_value()) {
        return op;
    }

    auto& info = patternInfo.value();

    // Build the SQL join query
    auto outputColumns = info.outputSchema->getExpressionsInScope();
    auto joinQuery = buildJoinQuery(info, outputColumns);

    // For now, log the detection and return original operator
    // The full implementation requires more work to properly wire up
    // the query execution path through the table function
    // TODO: Complete the createJoinTableFunctionCall implementation

    // Uncomment to enable the rewrite once fully implemented:
    // return createJoinTableFunctionCall(info, joinQuery);

    return op;
}

} // namespace optimizer
} // namespace lbug

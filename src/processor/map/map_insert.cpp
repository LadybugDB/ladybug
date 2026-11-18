#include "binder/expression/rel_expression.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/partition_routing.h"
#include "common/partition_routing_hook.h"
#include "common/string_utils.h"
#include "main/client_context.h"
#include "planner/operator/persistent/logical_insert.h"
#include "processor/expression_mapper.h"
#include "processor/operator/persistent/insert.h"
#include "processor/plan_mapper.h"
#include "storage/partition_storage_registry.h"
#include "storage/storage_manager.h"
#include "transaction/transaction.h"

using namespace lbug::evaluator;
using namespace lbug::planner;
using namespace lbug::storage;
using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::binder;

namespace lbug {
namespace processor {

static std::vector<DataPos> populateReturnColumnsPos(const LogicalInsertInfo& info,
    const Schema& schema) {
    std::vector<DataPos> result;
    for (auto i = 0u; i < info.columnDataExprs.size(); ++i) {
        if (info.isReturnColumnExprs[i]) {
            result.emplace_back(schema.getExpressionPos(*info.columnExprs[i]));
        } else {
            result.push_back(DataPos::getInvalidPos());
        }
    }
    return result;
}

// An explicitly named child is its own write target. A logical parent may bind
// to its own schema, one child, several children, or a wrapper-provided scan.
const NodeTableCatalogEntry* PlanMapper::getPartitionParent(const NodeExpression& node) const {
    if (node.isEmpty() || node.getEntry(0)->getType() != CatalogEntryType::NODE_TABLE_ENTRY) {
        return nullptr;
    }
    auto* first = node.getEntry(0)->ptrCast<NodeTableCatalogEntry>();
    if (first->isPartitioned()) {
        return node.getNumEntries() == 1 ? first : nullptr;
    }
    if (!first->isPartitionChild()) {
        return nullptr;
    }
    const auto parentID = first->getParentTableID();
    for (auto* entry : node.getEntries()) {
        if (entry->getType() != CatalogEntryType::NODE_TABLE_ENTRY ||
            entry->ptrCast<NodeTableCatalogEntry>()->getParentTableID() != parentID ||
            entry->getOwningCatalog() != first->getOwningCatalog()) {
            return nullptr;
        }
    }
    auto* catalog =
        first->getOwningCatalog() ? first->getOwningCatalog() : Catalog::Get(*clientContext);
    auto* parent =
        catalog->getTableCatalogEntry(transaction::Transaction::Get(*clientContext), parentID)
            ->ptrCast<NodeTableCatalogEntry>();
    if (node.isMultiLabeled() || first->getScanFunction().has_value() ||
        std::ranges::any_of(node.getOriginalLabels(), [&](const auto& label) {
            return StringUtils::caseInsensitiveEquals(label, parent->getName());
        })) {
        return parent;
    }
    return nullptr;
}

NodeInsertExecutor PlanMapper::getNodeInsertExecutor(const LogicalInsertInfo* boundInfo,
    const Schema& inSchema, const Schema& outSchema) const {
    auto& node = boundInfo->pattern->constCast<NodeExpression>();
    auto nodeIDPos = getDataPos(*node.getInternalID(), outSchema);
    auto columnsPos = populateReturnColumnsPos(*boundInfo, outSchema);
    auto info = NodeInsertInfo(nodeIDPos, columnsPos, boundInfo->conflictAction);
    evaluator_vector_t evaluators;
    auto exprMapper = ExpressionMapper(&inSchema);
    for (auto& expr : boundInfo->columnDataExprs) {
        evaluators.push_back(exprMapper.getEvaluator(expr));
    }
    const auto* firstEntry = node.getEntry(0)->ptrCast<NodeTableCatalogEntry>();
    const auto* parent = getPartitionParent(node);
    if (parent) {
        const auto parentID = parent->getTableID();
        const auto& childTableIDs = parent->getChildTableIDs();
        const auto* hooks = common::getPartitionRoutingHooks();
        auto tableInfo = NodeTableInsertInfo(nullptr, std::move(evaluators));
        tableInfo.partitionPKColumnID = parent->getColumnID(parent->getPrimaryKeyName());
        tableInfo.partitionKeyColumnID = parent->getPartitionColumnID();
        tableInfo.partitionMethod =
            static_cast<common::PartitionMethod>(*parent->getPartitionMethod());
        tableInfo.parentTableID = parentID;
        if (tableInfo.partitionMethod == common::PartitionMethod::HASH) {
            tableInfo.partitionTables.reserve(childTableIDs.size());
            for (auto i = 0u; i < childTableIDs.size(); ++i) {
                const auto ref = common::PartitionRef{parentID, i};
                common::PartitionHandle partHandle = nullptr;
                const bool claimed = hooks != nullptr && hooks->locate != nullptr &&
                                     hooks->locate(hooks->context, ref, &partHandle);
                tableInfo.partitionTables.push_back(
                    claimed ? nullptr :
                              storage::PartitionStorageRegistry::resolveNodeTableByID(clientContext,
                                  childTableIDs[i]));
                tableInfo.partitionChildIDs.push_back(childTableIDs[i]);
                tableInfo.partitionRefs.push_back(ref);
                tableInfo.partitionHandles.push_back(claimed ? partHandle : nullptr);
            }
        }
        return NodeInsertExecutor(std::move(info), std::move(tableInfo));
    }
    // Plain single-table write (including a direct pattern on one partition subgraph).
    auto table =
        storage::PartitionStorageRegistry::resolveNodeTable(clientContext, *node.getEntry(0));
    auto tableInfo = NodeTableInsertInfo(table, std::move(evaluators));
    if (firstEntry->isPartitionChild()) {
        tableInfo.partitionConstraint = firstEntry;
        auto* parent = Catalog::Get(*clientContext)
                           ->getTableCatalogEntry(transaction::Transaction::Get(*clientContext),
                               firstEntry->getParentTableID())
                           ->ptrCast<NodeTableCatalogEntry>();
        tableInfo.partitionKeyColumnID = parent->getPartitionColumnID();
    }
    return NodeInsertExecutor(std::move(info), std::move(tableInfo));
}

RelInsertExecutor PlanMapper::getRelInsertExecutor(const LogicalInsertInfo* boundInfo,
    const Schema& inSchema, const Schema& outSchema) const {
    auto& rel = boundInfo->pattern->constCast<RelExpression>();
    auto srcNode = rel.getSrcNode();
    auto dstNode = rel.getDstNode();
    auto srcNodeIDPos = getDataPos(*srcNode->getInternalID(), inSchema);
    auto dstNodeIDPos = getDataPos(*dstNode->getInternalID(), inSchema);
    auto columnsPos = populateReturnColumnsPos(*boundInfo, outSchema);
    auto info = RelInsertInfo(srcNodeIDPos, dstNodeIDPos, std::move(columnsPos));
    auto storageManager = StorageManager::Get(*clientContext);
    DASSERT(srcNode->getNumEntries() == 1 && dstNode->getNumEntries() == 1);
    auto srcTableID = srcNode->getEntry(0)->getTableID();
    auto dstTableID = dstNode->getEntry(0)->getTableID();
    DASSERT(rel.getNumEntries() == 1);
    auto& relGroupEntry = rel.getEntry(0)->constCast<RelGroupCatalogEntry>();
    auto relEntryInfo = relGroupEntry.getRelEntryInfo(srcTableID, dstTableID);
    auto table = storageManager->getTable(relEntryInfo->oid)->ptrCast<RelTable>();
    evaluator_vector_t evaluators;
    auto exprMapper = ExpressionMapper(&outSchema);
    for (auto& expr : boundInfo->columnDataExprs) {
        evaluators.push_back(exprMapper.getEvaluator(expr));
    }
    auto tableInfo = RelTableInsertInfo(table, std::move(evaluators));
    return RelInsertExecutor(std::move(info), std::move(tableInfo));
}

std::unique_ptr<PhysicalOperator> PlanMapper::mapInsert(const LogicalOperator* logicalOperator) {
    auto& logicalInsert = logicalOperator->constCast<LogicalInsert>();
    auto inSchema = logicalInsert.getChild(0)->getSchema();
    auto outSchema = logicalInsert.getSchema();
    auto prevOperator = mapOperator(logicalOperator->getChild(0).get());
    std::vector<NodeInsertExecutor> nodeExecutors;
    std::vector<RelInsertExecutor> relExecutors;
    for (auto& info : logicalInsert.getInfos()) {
        switch (info.tableType) {
        case TableType::NODE: {
            nodeExecutors.push_back(getNodeInsertExecutor(&info, *inSchema, *outSchema));
        } break;
        case TableType::REL: {
            relExecutors.push_back(getRelInsertExecutor(&info, *inSchema, *outSchema));
        } break;
        default:
            UNREACHABLE_CODE;
        }
    }
    expression_vector expressions;
    for (auto& info : logicalInsert.getInfos()) {
        for (auto& expr : info.columnExprs) {
            expressions.push_back(expr);
        }
    }
    auto printInfo =
        std::make_unique<InsertPrintInfo>(expressions, logicalInsert.getInfos()[0].conflictAction);
    return std::make_unique<Insert>(std::move(nodeExecutors), std::move(relExecutors),
        std::move(prevOperator), getOperatorID(), std::move(printInfo));
}

} // namespace processor
} // namespace lbug

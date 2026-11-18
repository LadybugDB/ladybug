#include "binder/ddl/bound_create_table_info.h"
#include "binder/expression/node_expression.h"
#include "binder/expression/rel_expression.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "planner/operator/persistent/logical_delete.h"
#include "processor/operator/persistent/delete.h"
#include "processor/plan_mapper.h"
#include "storage/partition_storage_registry.h"
#include "storage/storage_manager.h"

using namespace lbug::binder;
using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::planner;
using namespace lbug::storage;

namespace lbug {
namespace processor {

std::unique_ptr<NodeDeleteExecutor> PlanMapper::getNodeDeleteExecutor(
    const BoundDeleteInfo& boundInfo, const Schema& schema) const {
    DASSERT(boundInfo.tableType == TableType::NODE);
    auto& node = boundInfo.pattern->constCast<NodeExpression>();
    auto nodeIDPos = getDataPos(*node.getInternalID(), schema);
    auto info = NodeDeleteInfo(boundInfo.deleteType, nodeIDPos);
    if (node.isEmpty()) {
        return std::make_unique<EmptyNodeDeleteExecutor>(std::move(info));
    }
    table_id_map_t<NodeTableDeleteInfo> tableInfos;
    table_id_map_t<DataPos> parentPKPositions;
    auto* catalog = Catalog::Get(*clientContext);
    auto* transaction = transaction::Transaction::Get(*clientContext);
    for (auto* entry : node.getEntries()) {
        const auto& nodeEntry = entry->constCast<NodeTableCatalogEntry>();
        const auto tableID = nodeEntry.getTableID();
        const auto pkPos = getDataPos(*node.getPrimaryKey(tableID), schema);
        if (nodeEntry.isPartitioned()) {
            // An empty daily parent supplies its schema but has no storage.
            parentPKPositions.emplace(tableID, pkPos);
            continue;
        }
        tableInfos.emplace(tableID, NodeTableDeleteInfo::forTable(clientContext, tableID, pkPos));
        if (nodeEntry.isPartitionChild()) {
            const auto parentID = nodeEntry.getParentTableID();
            const auto& parent = catalog->getTableCatalogEntry(transaction, parentID)
                                     ->constCast<NodeTableCatalogEntry>();
            if (parent.getPartitionMethod() == BoundPartitionMethod::RANGE_DAY) {
                // CREATE may materialize a different day between planning and DELETE.
                parentPKPositions.emplace(parentID, pkPos);
            }
        }
    }
    if (tableInfos.size() == 1 && parentPKPositions.empty()) {
        return std::make_unique<SingleLabelNodeDeleteExecutor>(std::move(info),
            std::move(tableInfos.begin()->second));
    }
    return std::make_unique<MultiLabelNodeDeleteExecutor>(std::move(info), std::move(tableInfos),
        std::move(parentPKPositions));
}

std::unique_ptr<PhysicalOperator> PlanMapper::mapDelete(const LogicalOperator* logicalOperator) {
    auto delete_ = logicalOperator->constPtrCast<LogicalDelete>();
    switch (delete_->getTableType()) {
    case TableType::NODE: {
        return mapDeleteNode(logicalOperator);
    }
    case TableType::REL: {
        return mapDeleteRel(logicalOperator);
    }
    default:
        UNREACHABLE_CODE;
    }
}

std::unique_ptr<PhysicalOperator> PlanMapper::mapDeleteNode(
    const LogicalOperator* logicalOperator) {
    auto delete_ = logicalOperator->constPtrCast<LogicalDelete>();
    auto inSchema = delete_->getChild(0)->getSchema();
    auto prevOperator = mapOperator(logicalOperator->getChild(0).get());
    std::vector<std::unique_ptr<NodeDeleteExecutor>> executors;
    for (auto& info : delete_->getInfos()) {
        executors.push_back(getNodeDeleteExecutor(info, *inSchema));
    }
    expression_vector patterns;
    for (auto& info : delete_->getInfos()) {
        patterns.push_back(info.pattern);
    }
    auto printInfo =
        std::make_unique<DeleteNodePrintInfo>(patterns, delete_->getInfos()[0].deleteType);
    return std::make_unique<DeleteNode>(std::move(executors), std::move(prevOperator),
        getOperatorID(), std::move(printInfo));
}

std::unique_ptr<RelDeleteExecutor> PlanMapper::getRelDeleteExecutor(
    const BoundDeleteInfo& boundInfo, const Schema& schema) const {
    auto& rel = boundInfo.pattern->constCast<RelExpression>();
    if (rel.isEmpty()) {
        return std::make_unique<EmptyRelDeleteExecutor>();
    }
    auto relIDPos = getDataPos(*rel.getInternalID(), schema);
    auto srcNodeIDPos = getDataPos(*rel.getSrcNode()->getInternalID(), schema);
    auto dstNodeIDPos = getDataPos(*rel.getDstNode()->getInternalID(), schema);
    auto info = RelDeleteInfo(srcNodeIDPos, dstNodeIDPos, relIDPos);
    auto storageManager = StorageManager::Get(*clientContext);
    if (rel.isMultiLabeled()) {
        table_id_map_t<RelTable*> tableIDToTableMap;
        for (auto entry : rel.getEntries()) {
            auto& relGroupEntry = entry->constCast<RelGroupCatalogEntry>();
            for (auto& relEntryInfo : relGroupEntry.getRelEntryInfos()) {
                auto table = storageManager->getTable(relEntryInfo.oid);
                tableIDToTableMap.insert({table->getTableID(), table->ptrCast<RelTable>()});
            }
        }
        return std::make_unique<MultiLabelRelDeleteExecutor>(std::move(tableIDToTableMap),
            std::move(info));
    }
    DASSERT(rel.getNumEntries() == 1);
    auto& entry = rel.getEntry(0)->constCast<RelGroupCatalogEntry>();
    auto relEntryInfo = entry.getSingleRelEntryInfo();
    auto table = storageManager->getTable(relEntryInfo.oid)->ptrCast<RelTable>();
    return std::make_unique<SingleLabelRelDeleteExecutor>(table, std::move(info));
}

std::unique_ptr<PhysicalOperator> PlanMapper::mapDeleteRel(const LogicalOperator* logicalOperator) {
    auto delete_ = logicalOperator->constPtrCast<LogicalDelete>();
    auto inSchema = delete_->getChild(0)->getSchema();
    auto prevOperator = mapOperator(logicalOperator->getChild(0).get());
    std::vector<std::unique_ptr<RelDeleteExecutor>> executors;
    for (auto& info : delete_->getInfos()) {
        executors.push_back(getRelDeleteExecutor(info, *inSchema));
    }
    expression_vector patterns;
    for (auto& info : delete_->getInfos()) {
        patterns.push_back(info.pattern);
    }
    auto printInfo = std::make_unique<DeleteRelPrintInfo>(patterns);
    return std::make_unique<DeleteRel>(std::move(executors), std::move(prevOperator),
        getOperatorID(), std::move(printInfo));
}

} // namespace processor
} // namespace lbug

#include <algorithm>
#include <unordered_set>

#include "binder/expression/parameter_expression.h"
#include "binder/expression/property_expression.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "optimizer/filter_push_down_optimizer.h"
#include "planner/operator/scan/logical_scan_node_table.h"
#include "processor/partition_routing.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

using namespace lbug::binder;
using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace optimizer {

void FilterPushDownOptimizer::prunePartitions(LogicalScanNodeTable& scan) {
    if (scan.getPartitionEntries().empty()) {
        return;
    }
    auto tableIDs = scan.getTableIDs();
    const auto& nodeID = scan.getNodeID()->constCast<PropertyExpression>();
    auto* transaction = transaction::Transaction::Get(*context);
    std::unordered_set<table_id_t> excluded;
    for (const auto& predicate : predicateSet.equalityPredicates) {
        // Accept either orientation, but leave casts/functions of the property alone. In
        // particular, a lossy numeric comparison cannot safely determine a routing key.
        for (auto side = 0u; side < 2; ++side) {
            const auto& lhs = *predicate->getChild(side);
            const auto& rhs = *predicate->getChild(1 - side);
            if (lhs.expressionType != ExpressionType::PROPERTY ||
                !ExpressionUtil::canEvaluateAsLiteral(rhs)) {
                continue;
            }
            const auto& property = lhs.constCast<PropertyExpression>();
            if (property.getVariableName() != nodeID.getVariableName()) {
                continue;
            }
            // Compute the route once per parent, even when it has many children.
            std::unordered_map<const NodeTableCatalogEntry*, std::optional<table_id_t>> routes;
            for (const auto* child : scan.getPartitionEntries()) {
                auto* parent = child->getOwningCatalog()
                                   ->getTableCatalogEntry(transaction, child->getParentTableID())
                                   ->ptrCast<NodeTableCatalogEntry>();
                if (!parent->isPartitionPruningSafe() ||
                    property.getPropertyName() != parent->getPartitionColumnName()) {
                    continue;
                }
                const auto& keyType =
                    parent->getProperty(parent->getPartitionColumnName()).getType();
                if (lhs.getDataType() != keyType || rhs.getDataType() != keyType) {
                    continue;
                }
                const auto typeID = keyType.getLogicalTypeID();
                // Floating equality (NaNs and signed zero) need not imply identical hashes.
                if (typeID == LogicalTypeID::FLOAT || typeID == LogicalTypeID::DOUBLE) {
                    continue;
                }
                if (parent->getPartitionMethod() != BoundPartitionMethod::HASH &&
                    parent->getPartitionMethod() != BoundPartitionMethod::LIST) {
                    continue;
                }
                if (parent->getPartitionMethod() == BoundPartitionMethod::HASH &&
                    (parent->getNumPartitions() == 0 ||
                        parent->getChildTableIDs().size() != parent->getNumPartitions())) {
                    continue;
                }
                auto route = routes.find(parent);
                if (route == routes.end()) {
                    if (rhs.expressionType == ExpressionType::PARAMETER) {
                        // This value changes the set of physical tables. Replan on execution
                        // with new parameter values, just like parameterized SKIP/LIMIT.
                        rhs.constCast<ParameterExpression>().markBakedIntoPlan();
                    }
                    auto value = ExpressionUtil::evaluateAsLiteralValue(rhs);
                    std::optional<table_id_t> target;
                    if (!value.isNull()) {
                        ValueVector key{keyType.copy(), storage::MemoryManager::Get(*context),
                            DataChunkState::getSingleValueDataChunkState()};
                        key.copyFromValue(0, value);
                        if (parent->getPartitionMethod() == BoundPartitionMethod::HASH) {
                            std::vector<uint64_t> indexes;
                            processor::computePartitionIndexes(key, parent->getNumPartitions(),
                                indexes);
                            target = parent->getChildTableIDs()[indexes[0]];
                        } else {
                            target = parent->findListPartition(
                                processor::encodeListPartitionKey(key, 0));
                        }
                    }
                    route = routes.emplace(parent, target).first;
                }
                if (route->second != child->getTableID()) {
                    excluded.insert(child->getTableID());
                }
            }
        }
    }
    std::erase_if(tableIDs, [&](auto id) { return excluded.contains(id); });
    scan.setTableIDs(std::move(tableIDs));
}

} // namespace optimizer
} // namespace lbug

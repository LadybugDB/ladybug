#include <algorithm>
#include <limits>
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

// Intersect comparisons at the key's native precision before projecting onto days. Thus two
// contradictory bounds within one day select no child, and sub-day filters stay on the scan.
struct DayRangePredicate {
    int64_t lower = std::numeric_limits<int64_t>::min();
    int64_t upper = std::numeric_limits<int64_t>::max();

    void intersect(ExpressionType comparison, const Value& value) {
        if (value.isNull()) {
            lower = 1;
            upper = 0;
            return;
        }
        const int64_t tick = value.getDataType().getLogicalTypeID() == LogicalTypeID::DATE ?
                                 value.getValue<int32_t>() :
                                 value.getValue<int64_t>();
        switch (comparison) {
        case ExpressionType::EQUALS:
            lower = std::max(lower, tick);
            upper = std::min(upper, tick);
            break;
        case ExpressionType::LESS_THAN:
            if (tick == std::numeric_limits<int64_t>::min()) {
                lower = 1;
                upper = 0;
            } else {
                upper = std::min(upper, tick - 1);
            }
            break;
        case ExpressionType::LESS_THAN_EQUALS:
            upper = std::min(upper, tick);
            break;
        case ExpressionType::GREATER_THAN:
            if (tick == std::numeric_limits<int64_t>::max()) {
                lower = 1;
                upper = 0;
            } else {
                lower = std::max(lower, tick + 1);
            }
            break;
        case ExpressionType::GREATER_THAN_EQUALS:
            lower = std::max(lower, tick);
            break;
        default:
            UNREACHABLE_CODE;
        }
    }
};

static std::optional<table_id_t> equalityRoute(const NodeTableCatalogEntry& parent,
    const Value& value, main::ClientContext& context) {
    if (value.isNull()) {
        return std::nullopt;
    }
    ValueVector key{value.getDataType().copy(), storage::MemoryManager::Get(context),
        DataChunkState::getSingleValueDataChunkState()};
    key.copyFromValue(0, value);
    if (parent.getPartitionMethod() == BoundPartitionMethod::HASH) {
        std::vector<uint64_t> indexes;
        processor::computePartitionIndexes(key, parent.getNumPartitions(), indexes);
        return parent.getChildTableIDs()[indexes[0]];
    }
    return parent.findListPartition(processor::encodeListPartitionKey(key, 0));
}

void FilterPushDownOptimizer::prunePartitions(LogicalScanNodeTable& scan) {
    if (scan.getPartitionEntries().empty() || predicateSet.isEmpty()) {
        return;
    }
    const auto& nodeID = scan.getNodeID()->constCast<PropertyExpression>();
    auto* transaction = transaction::Transaction::Get(*context);
    std::unordered_map<const NodeTableCatalogEntry*, std::vector<const NodeTableCatalogEntry*>>
        childrenByParent;
    for (const auto* child : scan.getPartitionEntries()) {
        const auto* parent = child->getOwningCatalog()
                                 ->getTableCatalogEntry(transaction, child->getParentTableID())
                                 ->ptrCast<NodeTableCatalogEntry>();
        if (parent->isPartitionPruningSafe()) {
            childrenByParent[parent].push_back(child);
        }
    }

    std::unordered_set<table_id_t> excluded;
    const auto predicates = predicateSet.getAllPredicates();
    for (const auto& [parent, children] : childrenByParent) {
        const auto method = parent->getPartitionMethod();
        const bool daily = method == BoundPartitionMethod::RANGE_DAY;
        if (!daily && method != BoundPartitionMethod::HASH &&
            method != BoundPartitionMethod::LIST) {
            continue;
        }
        const auto& keyName = parent->getPartitionColumnName();
        const auto& keyType = parent->getProperty(keyName).getType();
        const auto typeID = keyType.getLogicalTypeID();
        // Floating equality (NaNs and signed zero) need not imply identical hashes.
        if (typeID == LogicalTypeID::FLOAT || typeID == LogicalTypeID::DOUBLE ||
            (method == BoundPartitionMethod::HASH &&
                (parent->getNumPartitions() == 0 ||
                    parent->getChildTableIDs().size() != parent->getNumPartitions()))) {
            continue;
        }
        std::optional<DayRangePredicate> dayRange;
        for (const auto& predicate : predicates) {
            auto comparison = predicate->expressionType;
            if (!ExpressionTypeUtil::isComparison(comparison) ||
                comparison == ExpressionType::NOT_EQUALS ||
                (!daily && comparison != ExpressionType::EQUALS)) {
                continue;
            }
            const Expression* lhs = predicate->getChild(0).get();
            const Expression* rhs = predicate->getChild(1).get();
            if (rhs->expressionType == ExpressionType::PROPERTY) {
                std::swap(lhs, rhs);
                comparison = ExpressionTypeUtil::reverseComparisonDirection(comparison);
            }
            // A cast or function of the property may not preserve its ordering or its routing key.
            if (lhs->expressionType != ExpressionType::PROPERTY ||
                !ExpressionUtil::canEvaluateAsLiteral(*rhs) || rhs->getDataType() != keyType ||
                lhs->getDataType() != keyType) {
                continue;
            }
            const auto& property = lhs->constCast<PropertyExpression>();
            if (property.getVariableName() != nodeID.getVariableName() ||
                property.getPropertyName() != keyName) {
                continue;
            }
            if (rhs->expressionType == ExpressionType::PARAMETER) {
                rhs->constCast<ParameterExpression>().markBakedIntoPlan();
            }
            const auto value = ExpressionUtil::evaluateAsLiteralValue(*rhs);
            if (daily) {
                if (!dayRange) {
                    dayRange.emplace();
                }
                dayRange->intersect(comparison, value);
            } else {
                const auto target = equalityRoute(*parent, value, *context);
                for (const auto* child : children) {
                    if (target != child->getTableID()) {
                        excluded.insert(child->getTableID());
                    }
                }
            }
        }
        if (dayRange) {
            const auto firstDay = processor::getPartitionDay(dayRange->lower, typeID);
            const auto lastDay = processor::getPartitionDay(dayRange->upper, typeID);
            for (const auto* child : children) {
                const auto day = processor::decodeDayPartitionKey(child->getListPartitionKey());
                if (dayRange->lower > dayRange->upper || day < firstDay || day > lastDay) {
                    excluded.insert(child->getTableID());
                }
            }
        }
    }
    auto tableIDs = scan.getTableIDs();
    std::erase_if(tableIDs, [&](auto id) { return excluded.contains(id); });
    scan.setTableIDs(std::move(tableIDs));
}

} // namespace optimizer
} // namespace lbug

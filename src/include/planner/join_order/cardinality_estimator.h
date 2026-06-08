#pragma once

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "binder/query/query_graph.h"
#include "common/enums/rel_direction.h"
#include "common/enums/table_type.h"
#include "planner/operator/logical_plan.h"
#include "storage/stats/planner_stats.h"

namespace lbug {
namespace main {
class ClientContext;
} // namespace main

namespace transaction {
class Transaction;
} // namespace transaction

namespace planner {

class LogicalAggregate;

using PlannerIndexStats = storage::PlannerIndexStats;
using PlannerRelDirectionStats = storage::PlannerRelDirectionStats;
using PlannerTableStats = storage::PlannerTableStats;

class CardinalityEstimator {
public:
    explicit CardinalityEstimator(main::ClientContext* context) : context{context} {}
    DELETE_COPY_DEFAULT_MOVE(CardinalityEstimator);

    void init(const binder::QueryGraph& queryGraph);
    LBUG_API void init(const binder::NodeExpression& node);
    LBUG_API void init(const binder::RelExpression& rel);

    void rectifyCardinality(const binder::Expression& nodeID, cardinality_t card);

    const PlannerTableStats* getTableStats(common::table_id_t tableID) const {
        return tableStats.contains(tableID) ? &tableStats.at(tableID) : nullptr;
    }
    const PlannerRelDirectionStats* getRelDirectionStats(common::table_id_t tableID,
        common::RelDataDirection direction) const {
        const auto stats = getTableStats(tableID);
        if (stats == nullptr) {
            return nullptr;
        }
        const auto directionKey = common::RelDirectionUtils::relDirectionToKeyIdx(direction);
        return stats->relDirectionStats[directionKey].has_value() ?
                   &stats->relDirectionStats[directionKey].value() :
                   nullptr;
    }

    cardinality_t estimateScanNode(const LogicalOperator& op) const;
    cardinality_t estimateHashJoin(const std::vector<binder::expression_pair>& joinConditions,
        const LogicalOperator& probeOp, const LogicalOperator& buildOp) const;
    cardinality_t estimateHashJoin(const binder::expression_vector& joinNodeIDs,
        const LogicalOperator& probeOp, const LogicalOperator& buildOp) const;
    cardinality_t estimateCrossProduct(const LogicalOperator& probeOp,
        const LogicalOperator& buildOp) const;
    cardinality_t estimateIntersect(const binder::expression_vector& joinNodeIDs,
        const LogicalOperator& probeOp, const std::vector<LogicalOperator*>& buildOps) const;
    cardinality_t estimateFlatten(const LogicalOperator& childOp,
        f_group_pos groupPosToFlatten) const;
    cardinality_t estimateFilter(const LogicalOperator& childOp,
        const binder::Expression& predicate) const;
    cardinality_t estimateAggregate(const LogicalAggregate& op) const;

    double getExtensionRate(const binder::RelExpression& rel,
        const binder::NodeExpression& boundNode, common::ExtendDirection direction,
        const transaction::Transaction* transaction) const;
    cardinality_t multiply(double extensionRate, cardinality_t card) const;

private:
    cardinality_t getNodeIDDom(const std::string& nodeIDName) const;
    cardinality_t getNumNodes(const transaction::Transaction* transaction,
        const std::vector<common::table_id_t>& tableIDs) const;
    cardinality_t getNumRels(const transaction::Transaction* transaction,
        const std::vector<common::table_id_t>& tableIDs) const;
    double getOneHopExtensionRate(const std::vector<common::table_id_t>& tableIDs,
        const std::vector<common::table_id_t>& boundTableIDs,
        common::RelDataDirection direction) const;

private:
    main::ClientContext* context;
    std::unordered_map<common::table_id_t, PlannerTableStats> tableStats;
    // The domain of nodeID is defined as the number of unique value of nodeID, i.e. num nodes.
    std::unordered_map<std::string, cardinality_t> nodeIDName2dom;
};

} // namespace planner
} // namespace lbug

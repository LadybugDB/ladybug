#pragma once

#include "binder/expression/expression.h"
#include "binder/expression/node_expression.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/enums/extend_direction.h"
#include "planner/operator/logical_operator.h"

namespace lbug {
namespace planner {

struct LogicalGroupedReachableCountPrintInfo final : OPPrintInfo {
    std::string relTableName;
    uint16_t lowerBound;
    uint16_t upperBound;
    size_t numKeys;
    bool hasCount;
    bool hasAvg;

    LogicalGroupedReachableCountPrintInfo(std::string relTableName, uint16_t lowerBound,
        uint16_t upperBound, size_t numKeys, bool hasCount, bool hasAvg)
        : relTableName{std::move(relTableName)}, lowerBound{lowerBound}, upperBound{upperBound},
          numKeys{numKeys}, hasCount{hasCount}, hasAvg{hasAvg} {}

    std::string toString() const override {
        return "Table: " + relTableName + ", Bounds: [" + std::to_string(lowerBound) + ".." +
               std::to_string(upperBound) + "], Keys: " + std::to_string(numKeys) +
               (hasCount ? ", COUNT" : "") + (hasAvg ? ", AVG" : "");
    }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<LogicalGroupedReachableCountPrintInfo>(relTableName, lowerBound,
            upperBound, numKeys, hasCount, hasAvg);
    }
};

/**
 * LogicalGroupedReachableCount answers a grouped aggregate over a variable-length path
 * (a)-[r*lo..up]->(b) without materializing the (a, b) walks.
 *
 * It replaces an AGGREGATE (GROUP BY properties of the end node `b`, with COUNT(*) / COUNT(b)
 * and at most one AVG(b.prop)) over a HASH_JOIN of a forward WALK RECURSIVE_EXTEND from the
 * full source table with a single operator that:
 *   1. counts, per distinct end node `b`, the number of walks of length in [lo, up] ending at
 *      `b` via level-by-level DP over the CSR (O(up * E) time, O(V) memory), and
 *   2. folds those per-node walk counts into the GROUP BY accumulators, reading each end
 *      node's properties once instead of once per walk.
 *
 * COUNT/SUM semantics weight every walk (row multiplicity), matching the unoptimized plan.
 * Created by CountRelTableOptimizer::tryRewriteGroupedReachableCount.
 */
class LogicalGroupedReachableCount final : public LogicalOperator {
    static constexpr LogicalOperatorType operatorType_ =
        LogicalOperatorType::GROUPED_REACHABLE_COUNT;

public:
    LogicalGroupedReachableCount(catalog::RelGroupCatalogEntry* relGroupEntry,
        std::shared_ptr<binder::NodeExpression> boundNode,
        std::shared_ptr<binder::NodeExpression> nbrNode, common::ExtendDirection direction,
        uint16_t lowerBound, uint16_t upperBound, binder::expression_vector keys,
        std::shared_ptr<binder::Expression> countExpr, std::shared_ptr<binder::Expression> avgExpr,
        std::shared_ptr<binder::Expression> avgChild, common::cardinality_t cardinality)
        : LogicalOperator{operatorType_}, relGroupEntry{relGroupEntry},
          boundNode{std::move(boundNode)}, nbrNode{std::move(nbrNode)}, direction{direction},
          lowerBound{lowerBound}, upperBound{upperBound}, keys{std::move(keys)},
          countExpr{std::move(countExpr)}, avgExpr{std::move(avgExpr)},
          avgChild{std::move(avgChild)} {
        this->cardinality = cardinality;
    }

    void computeFactorizedSchema() override;
    void computeFlatSchema() override;

    std::string getExpressionsForPrinting() const override;

    catalog::RelGroupCatalogEntry* getRelGroupEntry() const { return relGroupEntry; }
    std::shared_ptr<binder::NodeExpression> getBoundNode() const { return boundNode; }
    std::shared_ptr<binder::NodeExpression> getNbrNode() const { return nbrNode; }
    common::ExtendDirection getDirection() const { return direction; }
    uint16_t getLowerBound() const { return lowerBound; }
    uint16_t getUpperBound() const { return upperBound; }
    const binder::expression_vector& getKeys() const { return keys; }
    std::shared_ptr<binder::Expression> getCountExpr() const { return countExpr; }
    std::shared_ptr<binder::Expression> getAvgExpr() const { return avgExpr; }
    std::shared_ptr<binder::Expression> getAvgChild() const { return avgChild; }
    bool hasCount() const { return countExpr != nullptr; }
    bool hasAvg() const { return avgExpr != nullptr; }

    std::unique_ptr<OPPrintInfo> getPrintInfo() const override {
        return std::make_unique<LogicalGroupedReachableCountPrintInfo>(relGroupEntry->getName(),
            lowerBound, upperBound, keys.size(), hasCount(), hasAvg());
    }

    std::unique_ptr<LogicalOperator> copy() override {
        return std::make_unique<LogicalGroupedReachableCount>(relGroupEntry, boundNode, nbrNode,
            direction, lowerBound, upperBound, keys, countExpr, avgExpr, avgChild, cardinality);
    }

private:
    catalog::RelGroupCatalogEntry* relGroupEntry;
    std::shared_ptr<binder::NodeExpression> boundNode;
    std::shared_ptr<binder::NodeExpression> nbrNode;
    common::ExtendDirection direction;
    uint16_t lowerBound;
    uint16_t upperBound;
    binder::expression_vector keys;
    std::shared_ptr<binder::Expression> countExpr;
    std::shared_ptr<binder::Expression> avgExpr;
    std::shared_ptr<binder::Expression> avgChild;
};

} // namespace planner
} // namespace lbug

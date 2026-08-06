#pragma once

#include "binder/expression/expression.h"
#include "binder/expression/node_expression.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/enums/extend_direction.h"
#include "planner/operator/logical_operator.h"

namespace lbug {
namespace planner {

struct LogicalReachableCountPrintInfo final : OPPrintInfo {
    std::string relTableName;
    std::shared_ptr<binder::Expression> countExpr;
    uint16_t lowerBound;
    uint16_t upperBound;

    LogicalReachableCountPrintInfo(std::string relTableName,
        std::shared_ptr<binder::Expression> countExpr, uint16_t lowerBound, uint16_t upperBound)
        : relTableName{std::move(relTableName)}, countExpr{std::move(countExpr)},
          lowerBound{lowerBound}, upperBound{upperBound} {}

    std::string toString() const override {
        return "Table: " + relTableName + ", Count: " + countExpr->toString() + ", Bounds: [" +
               std::to_string(lowerBound) + ".." + std::to_string(upperBound) + "]";
    }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<LogicalReachableCountPrintInfo>(relTableName, countExpr, lowerBound,
            upperBound);
    }
};

/**
 * LogicalReachableCount is an optimized operator that computes COUNT(DISTINCT nbr) for a
 * variable-length path (a)-[r*lo..up]->(b) starting from a fixed source node (typically a single
 * node identified via a primary-key scan on a CSR-sorted node table). It replaces the
 * recursive-extend + hash-join subtree with a single operator that performs a bounded traversal
 * of the rel table and counts distinct reachable neighbor nodes at any depth within [lower, upper].
 *
 * This operator is created by CountRelTableOptimizer.
 */
class LogicalReachableCount final : public LogicalOperator {
    static constexpr LogicalOperatorType operatorType_ = LogicalOperatorType::REACHABLE_COUNT;

public:
    LogicalReachableCount(catalog::RelGroupCatalogEntry* relGroupEntry,
        std::shared_ptr<binder::NodeExpression> boundNode,
        std::shared_ptr<binder::NodeExpression> nbrNode, common::ExtendDirection direction,
        uint16_t lowerBound, uint16_t upperBound, std::shared_ptr<binder::Expression> countExpr,
        std::vector<common::offset_t> startOffsets)
        : LogicalOperator{operatorType_}, relGroupEntry{relGroupEntry},
          boundNode{std::move(boundNode)}, nbrNode{std::move(nbrNode)}, direction{direction},
          lowerBound{lowerBound}, upperBound{upperBound}, countExpr{std::move(countExpr)},
          startOffsets{std::move(startOffsets)} {
        cardinality = 1; // Always returns exactly one row.
    }

    void computeFactorizedSchema() override;
    void computeFlatSchema() override;

    std::string getExpressionsForPrinting() const override { return countExpr->toString(); }

    catalog::RelGroupCatalogEntry* getRelGroupEntry() const { return relGroupEntry; }
    std::shared_ptr<binder::NodeExpression> getBoundNode() const { return boundNode; }
    std::shared_ptr<binder::NodeExpression> getNbrNode() const { return nbrNode; }
    common::ExtendDirection getDirection() const { return direction; }
    uint16_t getLowerBound() const { return lowerBound; }
    uint16_t getUpperBound() const { return upperBound; }
    std::shared_ptr<binder::Expression> getCountExpr() const { return countExpr; }
    const std::vector<common::offset_t>& getStartOffsets() const { return startOffsets; }

    std::unique_ptr<OPPrintInfo> getPrintInfo() const override {
        return std::make_unique<LogicalReachableCountPrintInfo>(relGroupEntry->getName(), countExpr,
            lowerBound, upperBound);
    }

    std::unique_ptr<LogicalOperator> copy() override {
        return std::make_unique<LogicalReachableCount>(relGroupEntry, boundNode, nbrNode, direction,
            lowerBound, upperBound, countExpr, startOffsets);
    }

private:
    catalog::RelGroupCatalogEntry* relGroupEntry;
    std::shared_ptr<binder::NodeExpression> boundNode;
    std::shared_ptr<binder::NodeExpression> nbrNode;
    common::ExtendDirection direction;
    uint16_t lowerBound;
    uint16_t upperBound;
    std::shared_ptr<binder::Expression> countExpr;
    std::vector<common::offset_t> startOffsets;
};

} // namespace planner
} // namespace lbug

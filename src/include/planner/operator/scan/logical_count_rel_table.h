#pragma once

#include "binder/expression/expression.h"
#include "binder/expression/node_expression.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/enums/extend_direction.h"
#include "planner/operator/logical_operator.h"

namespace lbug {
namespace planner {

struct LogicalCountRelTablePrintInfo final : OPPrintInfo {
    std::string relTableName;
    std::shared_ptr<binder::Expression> countExpr;

    LogicalCountRelTablePrintInfo(std::string relTableName,
        std::shared_ptr<binder::Expression> countExpr)
        : relTableName{std::move(relTableName)}, countExpr{std::move(countExpr)} {}

    std::string toString() const override {
        return "Table: " + relTableName + ", Count: " + countExpr->toString();
    }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<LogicalCountRelTablePrintInfo>(relTableName, countExpr);
    }
};

/**
 * LogicalCountRelTable is an optimized operator that counts the number of rows
 * in a rel table by scanning through bound nodes and counting edges.
 *
 * This operator is created by CountRelTableOptimizer when it detects:
 *   COUNT(*) over a single rel table with no filters
 */
class LogicalCountRelTable final : public LogicalOperator {
    static constexpr LogicalOperatorType type_ = LogicalOperatorType::COUNT_REL_TABLE;

public:
    LogicalCountRelTable(catalog::RelGroupCatalogEntry* relGroupEntry,
        std::vector<common::table_id_t> relTableIDs,
        std::vector<common::table_id_t> boundNodeTableIDs,
        std::shared_ptr<binder::NodeExpression> boundNode, common::ExtendDirection direction,
        std::shared_ptr<binder::Expression> countExpr)
        : LogicalOperator{type_}, relGroupEntry{relGroupEntry}, relTableIDs{std::move(relTableIDs)},
          boundNodeTableIDs{std::move(boundNodeTableIDs)}, boundNode{std::move(boundNode)},
          direction{direction}, countExpr{std::move(countExpr)} {
        cardinality = 1; // Always returns exactly one row
    }

    void computeFactorizedSchema() override;
    void computeFlatSchema() override;

    std::string getExpressionsForPrinting() const override { return countExpr->toString(); }

    catalog::RelGroupCatalogEntry* getRelGroupEntry() const { return relGroupEntry; }
    const std::vector<common::table_id_t>& getRelTableIDs() const { return relTableIDs; }
    const std::vector<common::table_id_t>& getBoundNodeTableIDs() const {
        return boundNodeTableIDs;
    }
    std::shared_ptr<binder::NodeExpression> getBoundNode() const { return boundNode; }
    common::ExtendDirection getDirection() const { return direction; }
    std::shared_ptr<binder::Expression> getCountExpr() const { return countExpr; }

    std::unique_ptr<OPPrintInfo> getPrintInfo() const override {
        return std::make_unique<LogicalCountRelTablePrintInfo>(relGroupEntry->getName(), countExpr);
    }

    std::unique_ptr<LogicalOperator> copy() override {
        return std::make_unique<LogicalCountRelTable>(relGroupEntry, relTableIDs, boundNodeTableIDs,
            boundNode, direction, countExpr);
    }

private:
    catalog::RelGroupCatalogEntry* relGroupEntry;
    std::vector<common::table_id_t> relTableIDs;
    std::vector<common::table_id_t> boundNodeTableIDs;
    std::shared_ptr<binder::NodeExpression> boundNode;
    common::ExtendDirection direction;
    std::shared_ptr<binder::Expression> countExpr;
};

} // namespace planner
} // namespace lbug

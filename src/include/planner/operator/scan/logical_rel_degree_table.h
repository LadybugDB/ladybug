#pragma once

#include "binder/expression/expression.h"
#include "binder/expression/node_expression.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/enums/extend_direction.h"
#include "planner/operator/logical_operator.h"

namespace lbug {
namespace planner {

enum class RelDegreeTableMode : uint8_t { ACTIVE_BOUND_COUNT, TOP_K_DEGREES };

struct LogicalRelDegreeTablePrintInfo final : OPPrintInfo {
    std::string relTableName;
    RelDegreeTableMode mode;

    LogicalRelDegreeTablePrintInfo(std::string relTableName, RelDegreeTableMode mode)
        : relTableName{std::move(relTableName)}, mode{mode} {}

    std::string toString() const override {
        return "Table: " + relTableName + ", Mode: " +
               (mode == RelDegreeTableMode::ACTIVE_BOUND_COUNT ? "ACTIVE_BOUND_COUNT" :
                                                                 "TOP_K_DEGREES");
    }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<LogicalRelDegreeTablePrintInfo>(relTableName, mode);
    }
};

class LogicalRelDegreeTable final : public LogicalOperator {
    static constexpr LogicalOperatorType type_ = LogicalOperatorType::REL_DEGREE_TABLE;

public:
    LogicalRelDegreeTable(catalog::RelGroupCatalogEntry* relGroupEntry,
        std::vector<common::table_id_t> relTableIDs,
        std::shared_ptr<binder::NodeExpression> boundNode, common::ExtendDirection direction,
        RelDegreeTableMode mode, std::shared_ptr<binder::Expression> nodeKeyExpr,
        std::shared_ptr<binder::Expression> degreeExpr, common::idx_t limit)
        : LogicalOperator{type_}, relGroupEntry{relGroupEntry}, relTableIDs{std::move(relTableIDs)},
          boundNode{std::move(boundNode)}, direction{direction}, mode{mode},
          nodeKeyExpr{std::move(nodeKeyExpr)}, degreeExpr{std::move(degreeExpr)}, limit{limit} {
        cardinality = mode == RelDegreeTableMode::ACTIVE_BOUND_COUNT ? 1 : limit;
    }

    void computeFactorizedSchema() override;
    void computeFlatSchema() override;

    std::string getExpressionsForPrinting() const override {
        return mode == RelDegreeTableMode::ACTIVE_BOUND_COUNT ?
                   degreeExpr->toString() :
                   nodeKeyExpr->toString() + ", " + degreeExpr->toString();
    }

    catalog::RelGroupCatalogEntry* getRelGroupEntry() const { return relGroupEntry; }
    const std::vector<common::table_id_t>& getRelTableIDs() const { return relTableIDs; }
    std::shared_ptr<binder::NodeExpression> getBoundNode() const { return boundNode; }
    common::ExtendDirection getDirection() const { return direction; }
    RelDegreeTableMode getMode() const { return mode; }
    std::shared_ptr<binder::Expression> getNodeKeyExpr() const { return nodeKeyExpr; }
    std::shared_ptr<binder::Expression> getDegreeExpr() const { return degreeExpr; }
    common::idx_t getLimit() const { return limit; }

    std::unique_ptr<OPPrintInfo> getPrintInfo() const override {
        return std::make_unique<LogicalRelDegreeTablePrintInfo>(relGroupEntry->getName(), mode);
    }

    std::unique_ptr<LogicalOperator> copy() override {
        return std::make_unique<LogicalRelDegreeTable>(relGroupEntry, relTableIDs, boundNode,
            direction, mode, nodeKeyExpr, degreeExpr, limit);
    }

private:
    catalog::RelGroupCatalogEntry* relGroupEntry;
    std::vector<common::table_id_t> relTableIDs;
    std::shared_ptr<binder::NodeExpression> boundNode;
    common::ExtendDirection direction;
    RelDegreeTableMode mode;
    std::shared_ptr<binder::Expression> nodeKeyExpr;
    std::shared_ptr<binder::Expression> degreeExpr;
    common::idx_t limit;
};

} // namespace planner
} // namespace lbug

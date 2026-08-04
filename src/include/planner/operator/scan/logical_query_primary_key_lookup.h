#pragma once

#include "planner/operator/logical_operator.h"

namespace lbug {
namespace planner {

class LogicalQueryPrimaryKeyLookup final : public LogicalOperator {
    static constexpr LogicalOperatorType type_ = LogicalOperatorType::QUERY_PRIMARY_KEY_LOOKUP;

public:
    LogicalQueryPrimaryKeyLookup(common::table_id_t tableID,
        std::shared_ptr<binder::Expression> nodeID, binder::expression_vector properties,
        std::shared_ptr<binder::Expression> key, f_group_pos outputGroupPos,
        std::shared_ptr<LogicalOperator> child)
        : LogicalOperator{type_, std::move(child)}, tableID{tableID}, nodeID{std::move(nodeID)},
          properties{std::move(properties)}, key{std::move(key)}, outputGroupPos{outputGroupPos} {}

    void computeFactorizedSchema() override;
    void computeFlatSchema() override;
    std::string getExpressionsForPrinting() const override { return key->toString(); }

    common::table_id_t getTableID() const { return tableID; }
    const std::shared_ptr<binder::Expression>& getNodeID() const { return nodeID; }
    const binder::expression_vector& getProperties() const { return properties; }
    const std::shared_ptr<binder::Expression>& getKey() const { return key; }
    f_group_pos getOutputGroupPos() const { return outputGroupPos; }

    std::unique_ptr<LogicalOperator> copy() override {
        return make_unique<LogicalQueryPrimaryKeyLookup>(tableID, nodeID, properties, key,
            outputGroupPos, children[0]->copy());
    }

private:
    common::table_id_t tableID;
    std::shared_ptr<binder::Expression> nodeID;
    binder::expression_vector properties;
    std::shared_ptr<binder::Expression> key;
    f_group_pos outputGroupPos;
};

} // namespace planner
} // namespace lbug

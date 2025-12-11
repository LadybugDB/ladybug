#pragma once

#include "planner/operator/logical_plan.h"

namespace lbug {
namespace optimizer {

class OrderByPushDownOptimizer {
public:
    void rewrite(planner::LogicalPlan* plan);

private:
    std::shared_ptr<planner::LogicalOperator> visitOperator(
        std::shared_ptr<planner::LogicalOperator> op, std::string currentOrderBy = "");

    static std::string buildOrderByString(const binder::expression_vector& expressions,
        const std::vector<bool>& isAscOrders);
};

} // namespace optimizer
} // namespace lbug
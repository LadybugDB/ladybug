#pragma once

#include "function/table/bind_data.h"
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
    static std::string buildOrderByString(const binder::expression_vector& expressions,
        const std::vector<bool>& isAscOrders, const lbug::function::TableFuncBindData* target);
};

} // namespace optimizer
} // namespace lbug
#pragma once

#include "planner/operator/extend/logical_extend.h"

namespace lbug {
namespace planner {

class LogicalPackedExtend final : public LogicalExtend {
    static constexpr LogicalOperatorType type_ = LogicalOperatorType::PACKED_EXTEND;

public:
    LogicalPackedExtend(std::shared_ptr<binder::NodeExpression> boundNode,
        std::shared_ptr<binder::NodeExpression> nbrNode, std::shared_ptr<binder::RelExpression> rel,
        common::ExtendDirection direction, bool extendFromSource,
        binder::expression_vector properties, std::shared_ptr<LogicalOperator> child,
        common::cardinality_t cardinality = 0)
        : LogicalExtend{type_, std::move(boundNode), std::move(nbrNode), std::move(rel), direction,
              extendFromSource, std::move(properties), std::move(child), cardinality} {}

    f_group_pos_set getGroupsPosToFlatten() override { return f_group_pos_set{}; }
    void computeFactorizedSchema() override;
    void computeFlatSchema() override;

    std::unique_ptr<LogicalOperator> copy() override;
};

} // namespace planner
} // namespace lbug

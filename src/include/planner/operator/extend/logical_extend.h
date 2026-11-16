#pragma once

#include "planner/operator/extend/base_logical_extend.h"
#include "storage/predicate/column_predicate.h"

namespace lbug {
namespace planner {

class LogicalExtend : public BaseLogicalExtend {
    static constexpr LogicalOperatorType type_ = LogicalOperatorType::EXTEND;

public:
    LogicalExtend(std::shared_ptr<binder::NodeExpression> boundNode,
        std::shared_ptr<binder::NodeExpression> nbrNode, std::shared_ptr<binder::RelExpression> rel,
        common::ExtendDirection direction, bool extendFromSource,
        binder::expression_vector properties, std::shared_ptr<LogicalOperator> child,
        common::cardinality_t cardinality = 0)
        : BaseLogicalExtend{type_, std::move(boundNode), std::move(nbrNode), std::move(rel),
              direction, extendFromSource, std::move(child)},
          scanNbrID{true}, properties{std::move(properties)}, requireBoundFlatten{false} {
        this->cardinality = cardinality;
    }

    f_group_pos_set getGroupsPosToFlatten() override { return f_group_pos_set{}; }
    void computeFactorizedSchema() override;
    void computeFlatSchema() override;

    binder::expression_vector getProperties() const { return properties; }
    void setPropertyPredicates(std::vector<storage::ColumnPredicateSet> predicates) {
        propertyPredicates = std::move(predicates);
    }
    const std::vector<storage::ColumnPredicateSet>& getPropertyPredicates() const {
        return propertyPredicates;
    }
    void setScanNbrID(bool scanNbrID_) { scanNbrID = scanNbrID_; }
    bool shouldScanNbrID() const { return scanNbrID; }
    // Bound-expand (generalized lookup join) sets this so the optimizer keeps the
    // pre-extend FLATTEN: RemoveFactorizationRewriter strips every FLATTEN and
    // FactorizationRewriter only re-adds ones its visitors request. Without an
    // opt-in visitExtend the DISTINCT/AGGREGATE outer stays factorized at runtime
    // and the correlated CSR extend misreads batches under parallelism.
    void setRequireBoundFlatten(bool v) { requireBoundFlatten = v; }
    bool requireBoundFlattenFlag() const { return requireBoundFlatten; }

    std::unique_ptr<LogicalOperator> copy() override;

protected:
    LogicalExtend(LogicalOperatorType operatorType,
        std::shared_ptr<binder::NodeExpression> boundNode,
        std::shared_ptr<binder::NodeExpression> nbrNode, std::shared_ptr<binder::RelExpression> rel,
        common::ExtendDirection direction, bool extendFromSource,
        binder::expression_vector properties, std::shared_ptr<LogicalOperator> child,
        common::cardinality_t cardinality = 0)
        : BaseLogicalExtend{operatorType, std::move(boundNode), std::move(nbrNode), std::move(rel),
              direction, extendFromSource, std::move(child)},
          scanNbrID{true}, properties{std::move(properties)}, requireBoundFlatten{false} {
        this->cardinality = cardinality;
    }

private:
    bool scanNbrID;
    binder::expression_vector properties;
    std::vector<storage::ColumnPredicateSet> propertyPredicates;
    bool requireBoundFlatten;
};

} // namespace planner
} // namespace lbug

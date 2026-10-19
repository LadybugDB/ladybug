#pragma once

#include "column_predicate.h"
#include "common/enums/expression_type.h"

namespace lbug {
namespace storage {

class ColumnNullPredicate : public ColumnPredicate {
public:
    explicit ColumnNullPredicate(std::string columnName, common::ExpressionType type)
        : ColumnPredicate{std::move(columnName), type} {
        DASSERT(
            type == common::ExpressionType::IS_NULL || type == common::ExpressionType::IS_NOT_NULL);
    }

    common::ZoneMapCheckResult checkZoneMap(const MergedColumnChunkStats& stats) const override;

    void filter(const common::ValueVector& vector, common::SelectionVector& sel) const;

    std::unique_ptr<ColumnPredicate> copy() const override {
        return std::make_unique<ColumnNullPredicate>(columnName, expressionType);
    }
};

} // namespace storage
} // namespace lbug

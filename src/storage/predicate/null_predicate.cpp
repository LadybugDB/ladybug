#include "storage/predicate/null_predicate.h"

#include "common/vector/value_vector.h"
#include "storage/predicate/filter_selection.h"
#include "storage/table/column_chunk_stats.h"

namespace lbug::storage {
common::ZoneMapCheckResult ColumnNullPredicate::checkZoneMap(
    const MergedColumnChunkStats& mergedStats) const {
    const bool statToCheck = (expressionType == common::ExpressionType::IS_NULL) ?
                                 mergedStats.guaranteedNoNulls :
                                 mergedStats.guaranteedAllNulls;
    return statToCheck ? common::ZoneMapCheckResult::SKIP_SCAN :
                         common::ZoneMapCheckResult::ALWAYS_SCAN;
}

void ColumnNullPredicate::filter(const common::ValueVector& vector,
    common::SelectionVector& sel) const {
    const bool keepNulls = expressionType == common::ExpressionType::IS_NULL;
    filterSelection(sel, [&](common::sel_t pos) { return vector.isNull(pos) == keepNulls; });
}

} // namespace lbug::storage

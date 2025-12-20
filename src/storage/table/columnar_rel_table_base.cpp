#include "storage/table/columnar_rel_table_base.h"

#include <algorithm>

#include "common/exception/runtime.h"
#include "main/client_context.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::transaction;

namespace lbug {
namespace storage {

common::row_idx_t ColumnarRelTableBase::getNumTotalRows(const Transaction* transaction) {
    return getTotalRowCount(transaction);
}

common::offset_t ColumnarRelTableBase::findSourceNodeForRowInternal(offset_t globalRowIdx,
    const std::vector<offset_t>& indptrData) const {
    if (indptrData.empty()) {
        throw RuntimeException("Indptr data not loaded for CSR format");
    }

    // Binary search to find the source node
    // indptrData[i] contains the start row index for source node i
    // Find the largest i where indptrData[i] <= globalRowIdx
    auto it = std::upper_bound(indptrData.begin(), indptrData.end(), globalRowIdx);
    if (it == indptrData.begin()) {
        throw RuntimeException("Invalid global row index: " + std::to_string(globalRowIdx));
    }
    --it;
    return static_cast<offset_t>(std::distance(indptrData.begin(), it));
}

} // namespace storage
} // namespace lbug

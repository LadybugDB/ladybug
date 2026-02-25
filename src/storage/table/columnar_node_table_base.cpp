#include "storage/table/columnar_node_table_base.h"

#include "common/data_chunk/sel_vector.h"
#include "common/exception/runtime.h"
#include "main/client_context.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::transaction;

namespace lbug {
namespace storage {

void ColumnarNodeTableBase::initializeScanCoordination(const Transaction* transaction) {
    // Reset shared state at the start of each scan operation
    // This is called once per scan operation by the ScanNodeTable operator
    auto numBatches = getNumBatches(transaction);
    sharedState->reset(numBatches);
}

common::row_idx_t ColumnarNodeTableBase::getNumTotalRows(const Transaction* transaction) {
    return getTotalRowCount(transaction);
}

void ColumnarNodeTableBase::applySemiMaskFilter(const TableScanState& state,
    row_idx_t startNodeOffset, row_idx_t numRowsToScan, SelectionVector& selVector) const {
    if (!state.semiMask || !state.semiMask->isEnabled()) {
        return;
    }
    const auto endNodeOffset = startNodeOffset + numRowsToScan;
    const auto& arr = state.semiMask->range(startNodeOffset, endNodeOffset);
    if (arr.empty()) {
        selVector.setSelSize(0);
    } else {
        auto stat = selVector.getMutableBuffer();
        uint64_t numSelectedValues = 0;
        size_t i = 0, j = 0;
        while (i < numRowsToScan && j < arr.size()) {
            auto temp = arr[j] - startNodeOffset;
            if (selVector[i] < temp) {
                ++i;
            } else if (selVector[i] > temp) {
                ++j;
            } else {
                stat[numSelectedValues++] = temp;
                ++i;
                ++j;
            }
        }
        selVector.setToFiltered(numSelectedValues);
    }
}

} // namespace storage
} // namespace lbug

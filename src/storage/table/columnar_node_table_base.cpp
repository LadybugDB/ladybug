#include "storage/table/columnar_node_table_base.h"

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

} // namespace storage
} // namespace lbug

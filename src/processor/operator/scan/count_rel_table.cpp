#include "processor/operator/scan/count_rel_table.h"

#include "common/types/int128_t.h"
#include "common/types/uint128_t.h"
#include "main/client_context.h"
#include "processor/execution_context.h"
#include "storage/table/columnar_rel_table_base.h"
#include "storage/table/foreign_rel_table.h"
#include "storage/table/rel_table.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace processor {

void CountRelTable::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* /*context*/) {
    countVector = resultSet->getValueVector(countOutputPos).get();
    hasExecuted = false;
    totalCount = 0;
}

// Count rels from the rel table's CSR metadata instead of scanning every edge.
bool CountRelTable::getNextTuplesInternal(ExecutionContext* context) {
    if (hasExecuted) {
        return false;
    }

    auto transaction = Transaction::Get(*context->clientContext);

    for (auto* relTable : relTables) {
        if (dynamic_cast<ColumnarRelTableBase*>(relTable) != nullptr) {
            totalCount += relTable->getNumTotalRows(transaction);
            continue;
        }

        // Foreign-backed rel tables are scan-driven and own no CSR metadata.
        // Count via the scan function (ForeignRelTable::getNumTotalRows).
        if (dynamic_cast<ForeignRelTable*>(relTable) != nullptr) {
            totalCount += relTable->getNumTotalRows(transaction);
            continue;
        }

        totalCount += relTable->getNumVisibleRels(transaction, direction);
    }

    hasExecuted = true;

    // Write the count to the output vector (single value)
    countVector->state->getSelVectorUnsafe().setToUnfiltered(1);
    if (returnNullOnZero && totalCount == 0) {
        countVector->setNull(0, true);
        return true;
    }
    countVector->setNull(0, false);
    switch (countVector->dataType.getPhysicalType()) {
    case PhysicalTypeID::INT64:
        countVector->setValue<int64_t>(0, static_cast<int64_t>(totalCount));
        break;
    case PhysicalTypeID::INT128:
        countVector->setValue<int128_t>(0, int128_t{totalCount});
        break;
    case PhysicalTypeID::UINT128:
        countVector->setValue<uint128_t>(0, uint128_t{totalCount});
        break;
    default:
        UNREACHABLE_CODE;
    }

    return true;
}

} // namespace processor
} // namespace lbug

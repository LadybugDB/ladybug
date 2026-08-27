#include "storage/table/foreign_rel_table.h"

#include "function/table/table_function.h"
#include "processor/operator/scan/scan_rel_table.h"
#include "storage/storage_manager.h"
#include "transaction/transaction.h"
#include <format>

namespace lbug {
namespace storage {

ForeignRelTableScanState::ForeignRelTableScanState(MemoryManager& mm,
    common::ValueVector* nodeIDVector, std::vector<common::ValueVector*> outputVectors,
    std::shared_ptr<common::DataChunkState> outChunkState)
    : RelTableScanState{mm, nodeIDVector, std::move(outputVectors), std::move(outChunkState)} {
    dataChunk.valueVectors.resize(this->outputVectors.size());
    for (size_t i = 0; i < this->outputVectors.size(); ++i) {
        dataChunk.valueVectors[i] = std::shared_ptr<common::ValueVector>(this->outputVectors[i],
            [](common::ValueVector*) {});
    }
    dataChunk.state = this->outState;
}

ForeignRelTable::ForeignRelTable(catalog::RelGroupCatalogEntry* relGroupEntry,
    common::table_id_t fromTableID, common::table_id_t toTableID,
    const StorageManager* storageManager, MemoryManager* memoryManager,
    function::TableFunction scanFunction, std::shared_ptr<function::TableFuncBindData> scanBindData)
    : RelTable{relGroupEntry, fromTableID, toTableID, storageManager, memoryManager},
      scanFunction{std::move(scanFunction)}, scanBindData{std::move(scanBindData)} {}

void ForeignRelTable::initScanState([[maybe_unused]] transaction::Transaction* transaction,
    TableScanState& scanState, [[maybe_unused]] bool resetCachedBoundNodeSelVec) const {
    // For foreign tables, we don't need node group initialization
    // RelTable::initScanState(transaction, scanState, resetCachedBoundNodeSelVec);
    if (!scanBindData || scanFunction.tableFunc == nullptr ||
        scanFunction.initSharedStateFunc == nullptr || scanFunction.initLocalStateFunc == nullptr) {
        return;
    }
    auto& foreignRelScanState = static_cast<ForeignRelTableScanState&>(scanState);
    function::TableFuncInitSharedStateInput sharedInput{scanBindData.get(), nullptr /* context */};
    foreignRelScanState.sharedState = scanFunction.initSharedStateFunc(sharedInput);
    if (!foreignRelScanState.sharedState) {
        return;
    }
    function::TableFuncInitLocalStateInput localInput{*foreignRelScanState.sharedState,
        *scanBindData, nullptr /* clientContext */};
    foreignRelScanState.localState = scanFunction.initLocalStateFunc(localInput);
}

bool ForeignRelTable::scanInternal([[maybe_unused]] transaction::Transaction* transaction,
    [[maybe_unused]] TableScanState& scanState) {
    // Extending over a foreign-backed rel table requires translating the raw
    // foreign-key values returned by the scan function into lbug internal node
    // IDs. That mapping layer is not implemented yet, so a physical scan
    // cannot produce valid extend output (vectors are wired for node IDs, not
    // raw columns). Queries that can be rewritten by the foreign join
    // push-down optimizer never reach this path; everything else fails fast
    // instead of producing garbage or crashing.
    throw common::RuntimeException(
        std::format("MATCH traversal over foreign-backed rel table \"{}\" is not supported "
                    "yet: the foreign key to internal ID mapping layer is not implemented",
            tableName));
}

common::row_idx_t ForeignRelTable::getNumTotalRows(
    [[maybe_unused]] const transaction::Transaction* transaction) {
    // For foreign tables, we might need to query the foreign table for row count
    // For now, return 0 or implement proper counting
    return 0;
}

} // namespace storage
} // namespace lbug
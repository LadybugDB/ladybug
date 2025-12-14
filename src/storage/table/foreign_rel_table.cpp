#include "storage/table/foreign_rel_table.h"

#include "function/table/table_function.h"
#include "processor/operator/scan/scan_rel_table.h"
#include "storage/storage_manager.h"
#include "transaction/transaction.h"

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
    auto& foreignRelScanState = static_cast<ForeignRelTableScanState&>(scanState);
    function::TableFuncInitSharedStateInput sharedInput{scanBindData.get(), nullptr /* context */};
    foreignRelScanState.sharedState = scanFunction.initSharedStateFunc(sharedInput);
    function::TableFuncInitLocalStateInput localInput{*foreignRelScanState.sharedState,
        *scanBindData, nullptr /* clientContext */};
    foreignRelScanState.localState = scanFunction.initLocalStateFunc(localInput);
}

bool ForeignRelTable::scanInternal([[maybe_unused]] transaction::Transaction* transaction,
    TableScanState& scanState) {
    auto& foreignRelScanState = static_cast<ForeignRelTableScanState&>(scanState);
    function::TableFuncInput input{scanBindData.get(), foreignRelScanState.localState.get(),
        foreignRelScanState.sharedState.get(), nullptr /* clientContext */};
    common::DataChunk dc;
    dc.valueVectors = foreignRelScanState.dataChunk.valueVectors;
    dc.state = foreignRelScanState.dataChunk.state;
    function::TableFuncOutput output{std::move(dc)};
    auto numTuples = scanFunction.tableFunc(input, output);
    return numTuples > 0;
}

common::row_idx_t ForeignRelTable::getNumTotalRows(
    [[maybe_unused]] const transaction::Transaction* transaction) {
    // For foreign tables, we might need to query the foreign table for row count
    // For now, return 0 or implement proper counting
    return 0;
}

} // namespace storage
} // namespace lbug
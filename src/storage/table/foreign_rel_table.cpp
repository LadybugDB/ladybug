#include "storage/table/foreign_rel_table.h"

#include <unordered_set>

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
      relGroupEntry{relGroupEntry}, scanFunction{std::move(scanFunction)},
      scanBindData{std::move(scanBindData)} {}

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

// Raw foreign key values must be integers to be usable as node offsets.
static int64_t readOffsetValue(const common::ValueVector& vector, common::sel_t pos) {
    switch (vector.dataType.getLogicalTypeID()) {
    case common::LogicalTypeID::INT64:
        return vector.getValue<int64_t>(pos);
    case common::LogicalTypeID::INT32:
        return vector.getValue<int32_t>(pos);
    default:
        throw common::RuntimeException(
            std::format("Foreign key columns of foreign-backed rel tables must be INT32 or INT64 "
                        "to be usable as node offsets, got {}",
                vector.dataType.toString()));
    }
}

bool ForeignRelTable::scanInternal([[maybe_unused]] transaction::Transaction* transaction,
    TableScanState& scanState) {
    auto& foreignRelScanState = static_cast<ForeignRelTableScanState&>(scanState);
    if (!scanBindData || scanFunction.tableFunc == nullptr) {
        throw common::RuntimeException(
            std::format("Cannot scan foreign-backed rel table \"{}\" without a bound scan function",
                tableName));
    }
    // The csr_rel_* contract: the src/dst scan columns contain node offsets
    // directly (foreign keys usable as table offsets by design), so the raw
    // values can be wrapped into internal node IDs without a mapping layer.
    const auto srcDstPos = scanBindData->getSrcDstColumnPositions();
    if (!srcDstPos) {
        throw common::RuntimeException(
            std::format("MATCH traversal over foreign-backed rel table \"{}\" is not supported: "
                        "the foreign keys cannot be interpreted as node offsets. Register the "
                        "table with the csr_rel_ prefix to promise offset-compatible foreign keys",
                tableName));
    }
    if (foreignRelScanState.sharedState == nullptr) {
        throw common::RuntimeException(std::format(
            "Cannot scan foreign-backed rel table \"{}\": scan state was not initialized",
            tableName));
    }

    // Scan the foreign table into an internal chunk (one vector per scan
    // output column), then distribute rows into the operator's vectors.
    const auto numScanColumns = scanBindData->getNumColumns();
    auto chunkState = std::make_shared<common::DataChunkState>();
    common::DataChunk chunk(numScanColumns, chunkState);
    for (auto i = 0u; i < numScanColumns; i++) {
        chunk.valueVectors[i] = std::make_shared<common::ValueVector>(
            scanBindData->columns[i]->dataType.copy(), memoryManager, chunkState);
    }

    const bool fwd = foreignRelScanState.direction == common::RelDataDirection::FWD;
    const auto nbrTableID = fwd ? getToNodeTableID() : getFromNodeTableID();

    // Bound node offsets for the current input batch. The operator fills
    // nodeIDVector before each initScanState; extend output must be limited
    // to edges whose bound-side endpoint is in this batch.
    std::unordered_set<common::offset_t> boundOffsets;
    if (scanState.nodeIDVector != nullptr) {
        auto& boundVec = *scanState.nodeIDVector;
        const auto& boundSel = boundVec.state->getSelVector();
        for (auto i = 0u; i < boundSel.getSelSize(); i++) {
            boundOffsets.insert(boundVec.getValue<common::nodeID_t>(boundSel[i]).offset);
        }
    }

    // Map operator output columns (columnIDs[i] >= 1) to scan column positions.
    std::vector<common::column_id_t> outToScanCol(scanState.outputVectors.size(),
        common::INVALID_COLUMN_ID);
    for (auto i = 1u; i < scanState.outputVectors.size(); i++) {
        const auto columnID = scanState.columnIDs[i];
        if (columnID == common::INVALID_COLUMN_ID) {
            continue;
        }
        auto scanPos = 0u;
        for (auto& property : relGroupEntry->getProperties()) {
            if (relGroupEntry->getColumnID(property.getName()) == columnID) {
                outToScanCol[i] = scanPos;
                break;
            }
            scanPos++;
        }
    }

    auto numEmitted = 0u;
    for (;;) {
        common::DataChunk dc;
        dc.valueVectors = chunk.valueVectors;
        dc.state = chunkState;
        function::TableFuncOutput output{std::move(dc)};
        function::TableFuncInput input{scanBindData.get(), foreignRelScanState.localState.get(),
            foreignRelScanState.sharedState.get(), nullptr /* clientContext */};
        const auto numTuples = scanFunction.tableFunc(input, output);
        if (numTuples == 0) {
            break;
        }
        // FWD scan: bound side is the src column, nbr side the dst column.
        // BWD scan: reversed.
        const auto boundPos = fwd ? srcDstPos->first : srcDstPos->second;
        const auto nbrPos = fwd ? srcDstPos->second : srcDstPos->first;
        auto& boundVec = *chunk.valueVectors[boundPos];
        auto& nbrVec = *chunk.valueVectors[nbrPos];
        for (common::sel_t r = 0; r < numTuples; r++) {
            if (boundVec.isNull(r) || nbrVec.isNull(r)) {
                continue;
            }
            const auto boundOffset = readOffsetValue(boundVec, r);
            if (!boundOffsets.empty() && !boundOffsets.contains(boundOffset)) {
                continue;
            }
            const auto nbrOffset = readOffsetValue(nbrVec, r);
            // Wrap the raw foreign key value as the node offset (by design).
            scanState.outputVectors[0]->setValue<common::nodeID_t>(numEmitted,
                common::nodeID_t{static_cast<common::offset_t>(nbrOffset), nbrTableID});
            for (auto i = 1u; i < scanState.outputVectors.size(); i++) {
                if (outToScanCol[i] == common::INVALID_COLUMN_ID) {
                    continue;
                }
                scanState.outputVectors[i]->copyFromVectorData(numEmitted,
                    chunk.valueVectors[outToScanCol[i]].get(), r);
            }
            numEmitted++;
        }
        if (numEmitted > 0) {
            break;
        }
    }
    if (numEmitted == 0) {
        return false;
    }
    foreignRelScanState.outState->getSelVectorUnsafe().setToUnfiltered(numEmitted);
    return true;
}

common::row_idx_t ForeignRelTable::getNumTotalRows(
    [[maybe_unused]] const transaction::Transaction* transaction) {
    // For foreign tables, we might need to query the foreign table for row count
    // For now, return 0 or implement proper counting
    return 0;
}

} // namespace storage
} // namespace lbug
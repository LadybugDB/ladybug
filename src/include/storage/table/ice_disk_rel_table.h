#pragma once

#include <cstdint>

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/types/value/value.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"
#include "storage/table/rel_table.h"

namespace lbug {
namespace storage {

class IceDiskRelTable;

struct PendingIceDiskRelRow {
    common::sel_t boundNodeSelPos = 0;
    common::nodeID_t nbrNodeID;
    common::internalID_t relID;
    std::vector<std::unique_ptr<common::Value>> propertyValues;

    PendingIceDiskRelRow() = default;
    PendingIceDiskRelRow(PendingIceDiskRelRow&&) = default;
    PendingIceDiskRelRow& operator=(PendingIceDiskRelRow&&) = default;
};

struct IceDiskRelTableScanState : public RelTableScanState {
    std::unique_ptr<processor::ParquetReader> indicesReader;
    std::unique_ptr<processor::ParquetReaderScanState> parquetScanState;
    std::vector<int64_t> outputColumnIdx;
    std::vector<bool> columnSkips;
    bool scanCompleted = false;
    uint64_t currentStartRow = 0;
    uint64_t currentNumRows = 0;
    uint64_t currentGlobalRowIdx = 0;
    uint64_t nextRowGroupIdx = 0;
    std::vector<PendingIceDiskRelRow> pendingRows;
    uint64_t nextPendingRowIdx = 0;

    IceDiskRelTableScanState(MemoryManager& mm, common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : RelTableScanState{mm, nodeIDVector, std::move(outputVectors), std::move(outChunkState)} {
        parquetScanState = std::make_unique<processor::ParquetReaderScanState>();
    }

    void setToTable(const transaction::Transaction* transaction, Table* table_,
        std::vector<common::column_id_t> columnIDs_,
        std::vector<ColumnPredicateSet> columnPredicateSets_ = {},
        common::RelDataDirection direction_ = common::RelDataDirection::FWD) override;
};

struct IceDiskRelTableScanSharedState {
private:
    std::mutex mtx;
    std::vector<size_t> rowGroupStartRows;
    std::vector<size_t> rowGroupNumRows;
    common::node_group_idx_t currentRowGroupIdx = 0;

public:
    IceDiskRelTableScanSharedState() {}

    void reset(std::vector<size_t> startRows, std::vector<size_t> numRows) {
        std::lock_guard<std::mutex> lock(mtx);
        this->rowGroupStartRows = std::move(startRows);
        this->rowGroupNumRows = std::move(numRows);
        this->currentRowGroupIdx = 0;
    }

    bool getNextMorsel(IceDiskRelTableScanState* scanState, uint64_t& startRow, uint64_t& numRows) {
        std::lock_guard<std::mutex> lock(mtx);
        if (currentRowGroupIdx < rowGroupStartRows.size()) {
            scanState->nodeGroupIdx = currentRowGroupIdx;
            startRow = rowGroupStartRows[currentRowGroupIdx];
            numRows = rowGroupNumRows[currentRowGroupIdx];
            currentRowGroupIdx++;
            return true;
        }
        return false;
    }

    bool getMorsel(common::node_group_idx_t morselIdx, uint64_t& startRow, uint64_t& numRows) {
        std::lock_guard<std::mutex> lock(mtx);
        if (morselIdx >= rowGroupStartRows.size()) {
            return false;
        }
        startRow = rowGroupStartRows[morselIdx];
        numRows = rowGroupNumRows[morselIdx];
        return true;
    }

    common::node_group_idx_t getNumMorsels() const { return rowGroupStartRows.size(); }
};

class IceDiskRelTable final : public RelTable {
public:
    IceDiskRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
        common::table_id_t toTableID, const StorageManager* storageManager,
        MemoryManager* memoryManager);

    void initializeScanCoordination(const transaction::Transaction* transaction);

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

    void insert(transaction::Transaction*, TableInsertState&) override {
        throw common::RuntimeException("Cannot insert into icebug-disk-backed rel table");
    }
    void update(transaction::Transaction*, TableUpdateState&) override {
        throw common::RuntimeException("Cannot update icebug-disk-backed rel table");
    }
    bool delete_(transaction::Transaction*, TableDeleteState&) override {
        throw common::RuntimeException("Cannot delete from icebug-disk-backed rel table");
    }

    common::row_idx_t getNumTotalRows(const transaction::Transaction* transaction) override;

    const std::string& getIndicesFilePath() const { return indicesFilePath; }
    const std::string& getIndptrFilePath() const { return indptrFilePath; }
    const catalog::RelGroupCatalogEntry* getRelGroupCatalogEntry() const { return relGroupCatalogEntry; }
    IceDiskRelTableScanSharedState* getTableScanSharedState() const { return tableScanSharedState.get(); }

private:
    std::string indicesFilePath;
    std::string indptrFilePath;
    const catalog::RelGroupCatalogEntry* relGroupCatalogEntry;
    mutable std::unique_ptr<IceDiskRelTableScanSharedState> tableScanSharedState;
    mutable std::mutex indptrDataMutex;
    mutable std::vector<common::offset_t> indptrData;

    void loadIndptrData(transaction::Transaction* transaction) const;
    common::offset_t findSourceNodeForRow(common::offset_t globalRowIdx) const;
};

} // namespace storage
} // namespace lbug

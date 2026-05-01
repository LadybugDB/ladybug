#pragma once

#include <mutex>
#include <vector>

#include "storage/table/node_table.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"

namespace lbug {
namespace storage {

class IceDiskNodeTable;

struct IceDiskNodeTableScanState : public TableScanState {
    std::unique_ptr<processor::ParquetReader> parquetReader;
    std::unique_ptr<processor::ParquetReaderScanState> parquetScanState;
    std::vector<bool> columnSkips;
    bool scanCompleted = false;
    uint64_t currentStartRow = 0;
    uint64_t currentNumRows = 0;
    uint64_t currentRowOffset = 0;

    IceDiskNodeTableScanState(common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : TableScanState{nodeIDVector, std::move(outputVectors), std::move(outChunkState)} {
        parquetScanState = std::make_unique<processor::ParquetReaderScanState>();
    }

    void setToTable(const transaction::Transaction* transaction, Table* table_,
        std::vector<common::column_id_t> columnIDs_,
        std::vector<ColumnPredicateSet> columnPredicateSets_ = {},
        common::RelDataDirection direction = common::RelDataDirection::FWD) override;
};

struct IceDiskNodeTableScanSharedState {
private:
    std::mutex mtx;
    std::vector<size_t> rowGroupRows;
    std::vector<size_t> rowGroupStartRows;
    common::node_group_idx_t currentMorselIdx = 0;

public:
    IceDiskNodeTableScanSharedState() {}

    void reset(std::vector<size_t> rows, std::vector<size_t> startRows) {
        std::lock_guard<std::mutex> lock(mtx);
        this->rowGroupRows = std::move(rows);
        this->rowGroupStartRows = std::move(startRows);
        this->currentMorselIdx = 0;
    }

    bool getNextMorsel(IceDiskNodeTableScanState* scanState) {
        std::lock_guard<std::mutex> lock(mtx);
        if (currentMorselIdx < rowGroupRows.size()) {
            scanState->nodeGroupIdx = currentMorselIdx;
            scanState->currentNumRows = rowGroupRows[currentMorselIdx];
            scanState->currentStartRow = rowGroupStartRows[currentMorselIdx];
            currentMorselIdx++;
            return true;
        }
        return false;
    }

    void resetMorsel() {
        std::lock_guard<std::mutex> lock(mtx);
        currentMorselIdx = 0;
    }

    size_t getNumMorsels() const { return rowGroupRows.size(); }
};

class IceDiskNodeTable final : public NodeTable {
public:
    IceDiskNodeTable(const StorageManager* storageManager,
        const catalog::NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager);

    void initializeScanCoordination(const transaction::Transaction* transaction) override;

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

    void insert(transaction::Transaction*, TableInsertState&) override {
        throw common::RuntimeException("Cannot insert into icebug-disk-backed node table");
    }
    void update(transaction::Transaction*, TableUpdateState&) override {
        throw common::RuntimeException("Cannot update icebug-disk-backed node table");
    }
    bool delete_(transaction::Transaction*, TableDeleteState&) override {
        throw common::RuntimeException("Cannot delete from icebug-disk-backed node table");
    }

    common::row_idx_t getNumTotalRows(const transaction::Transaction* transaction) override;

    const std::string& getParquetFilePath() const { return parquetFilePath; }
    const catalog::NodeTableCatalogEntry* getCatalogEntry() const { return nodeTableCatalogEntry; }
    IceDiskNodeTableScanSharedState* getTableScanSharedState() const { return tableScanSharedState.get(); }

private:
    std::string parquetFilePath;
    const catalog::NodeTableCatalogEntry* nodeTableCatalogEntry;
    std::unique_ptr<IceDiskNodeTableScanSharedState> tableScanSharedState;
};

} // namespace storage
} // namespace lbug

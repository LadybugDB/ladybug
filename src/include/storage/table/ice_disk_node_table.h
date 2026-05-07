#pragma once

#include <atomic>
#include <vector>

#include "storage/table/node_table.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"

namespace lbug {
namespace storage {

class IceDiskNodeTable;

struct IceDiskNodeTableScanState : public TableScanState {
    bool initialized = false;
    std::unique_ptr<processor::ParquetReader> parquetReader;
    std::unique_ptr<processor::ParquetReaderScanState> parquetScanState;
    bool scanCompleted = false;
    std::size_t currentRowGroupIdx = static_cast<std::size_t>(common::INVALID_NODE_GROUP_IDX);
    bool dataReadCompleted = false;
    std::vector<std::vector<std::unique_ptr<common::Value>>> data; // data[rowGroup][column]
    std::size_t currentRowGroupBatchOffset = 0; // offset of current rowGroupBatch


    IceDiskNodeTableScanState(common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : TableScanState{nodeIDVector, std::move(outputVectors), std::move(outChunkState)} {
        parquetScanState = std::make_unique<processor::ParquetReaderScanState>();
    }

    void setToTable(const transaction::Transaction* transaction, Table* table_,
        std::vector<common::column_id_t> columnIDs_,
        std::vector<ColumnPredicateSet> columnPredicateSets_ = {},
        common::RelDataDirection direction = common::RelDataDirection::INVALID) override;
};

// Shared state for morsel assignment across parallel scan threads
struct IceDiskNodeTableScanSharedState {
private:
    std::mutex mtx;
    std::size_t numRowGroups = 0;
    std::size_t currentRowGroupIdx = 0;

public:
    void reset(std::size_t totalRowGroups) {
        numRowGroups = totalRowGroups;
        currentRowGroupIdx = 0;
    }

    bool getNextMorsel(IceDiskNodeTableScanState* scanState) {
        std::lock_guard<std::mutex> lock(mtx);

        if (currentRowGroupIdx < numRowGroups) {
            scanState->currentRowGroupIdx = currentRowGroupIdx;
            currentRowGroupIdx++;
            return true;
        }

        return false;
    }
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

    std::size_t getNumScanMorsels(const transaction::Transaction* transaction) const;

private:
    std::size_t getNumRowGroups(const transaction::Transaction* transaction) const;
    void initIceDiskScanForRowGroup(transaction::Transaction* transaction, IceDiskNodeTableScanState& scanState) const;
    void readParquetData(transaction::Transaction* transaction, TableScanState& scanState) const;

private:
    std::string parquetFilePath;
    const catalog::NodeTableCatalogEntry* nodeTableCatalogEntry;
    std::vector<std::size_t> rowGroupStartOffsets;
    mutable std::unique_ptr<IceDiskNodeTableScanSharedState> tableScanSharedState;
    constexpr static std::size_t scanRowGroupBatchSize = 2048; // Default batch size
};

} // namespace storage
} // namespace lbug

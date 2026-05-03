#pragma once

#include <mutex>
#include <vector>

#include "storage/table/node_table.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"

namespace lbug {
namespace storage {

class IceDiskNodeTable;

struct IceDiskNodeTableScanSharedState {
private:
    std::mutex mtx;
    common::node_group_idx_t currentRowGroupIdx = 0;
    common::node_group_idx_t numRowGroups = 0;

public:
    void reset(common::node_group_idx_t totalRowGroups) {
        std::lock_guard<std::mutex> lock(mtx);
        currentRowGroupIdx = 0;
        numRowGroups = totalRowGroups;
    }


    bool getNextMorsel(IceDiskNodeTableScanState* scanState) {
        std::lock_guard<std::mutex> lock(mtx);
        if (currentRowGroupIdx < numRowGroups) {
            scanState->currentRowGroupIdx = currentRowGroupIdx++;
            return true;
        }
        return false;
     }
};

struct IceDiskNodeTableScanState : public TableScanState {
    std::unique_ptr<processor::ParquetReader> parquetReader;
    std::unique_ptr<processor::ParquetReaderScanState> parquetScanState;
    std::vector<bool> columnSkips;
    bool scanCompleted = false;
    common::node_group_idx_t currentRowGroupIdx = 0;


    IceDiskNodeTableScanState(common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : TableScanState{nodeIDVector, std::move(outputVectors), std::move(outChunkState)} {
        parquetScanState = std::make_unique<processor::ParquetReaderScanState>();
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

    size_t getNumScanMorsels(const transaction::Transaction* transaction) const;

private:
    size_t getNumRowGroups(const transaction::Transaction* transaction) const;

private:
    std::string parquetFilePath;
    const catalog::NodeTableCatalogEntry* nodeTableCatalogEntry;
    std::unique_ptr<IceDiskNodeTableScanSharedState> tableScanSharedState;
};

} // namespace storage
} // namespace lbug

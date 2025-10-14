#pragma once

#include <mutex>
#include <vector>

#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/types/internal_id_util.h"
#include "common/types/value/value.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"
#include "storage/table/node_table.h"

namespace lbug {
namespace storage {

struct ParquetNodeTableScanState final : NodeTableScanState {
    std::unique_ptr<processor::ParquetReader> parquetReader;
    std::unique_ptr<processor::ParquetReaderScanState> parquetScanState;
    bool initialized = false;
    bool scanCompleted = false; // Track if this scan state has finished reading
    bool dataRead = false;
    std::vector<std::vector<std::unique_ptr<common::Value>>> allData;
    size_t totalRows = 0;
    size_t nextRowToDistribute = 0;
    uint64_t lastQueryId = 0; // Track the last query ID to detect new queries

    ParquetNodeTableScanState([[maybe_unused]] MemoryManager& mm, common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : NodeTableScanState{nodeIDVector, std::move(outputVectors), std::move(outChunkState)} {
        parquetScanState = std::make_unique<processor::ParquetReaderScanState>();
    }
};

// Shared state to coordinate row group assignment across parallel scan states
struct ParquetNodeTableSharedState {
    std::mutex mtx;
    common::node_group_idx_t currentRowGroupIdx = 0;
    common::node_group_idx_t numRowGroups = 0;

    void reset(common::node_group_idx_t totalRowGroups) {
        std::lock_guard<std::mutex> lock(mtx);
        currentRowGroupIdx = 0;
        numRowGroups = totalRowGroups;
    }

    bool getNextRowGroup(common::node_group_idx_t& assignedRowGroupIdx) {
        std::lock_guard<std::mutex> lock(mtx);
        if (currentRowGroupIdx < numRowGroups) {
            assignedRowGroupIdx = currentRowGroupIdx++;
            return true;
        }
        return false;
    }
};

class ParquetNodeTable final : public NodeTable {
public:
    ParquetNodeTable(const StorageManager* storageManager,
        const catalog::NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager);

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    // Override to reset shared state for row group coordination at the start of each scan operation
    void initializeScanCoordination(const transaction::Transaction* transaction) override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

    // For parquet-backed tables, we don't support modifications
    void insert([[maybe_unused]] transaction::Transaction* transaction,
        [[maybe_unused]] TableInsertState& insertState) override {
        throw common::RuntimeException("Cannot insert into parquet-backed node table");
    }
    void update([[maybe_unused]] transaction::Transaction* transaction,
        [[maybe_unused]] TableUpdateState& updateState) override {
        throw common::RuntimeException("Cannot update parquet-backed node table");
    }
    bool delete_([[maybe_unused]] transaction::Transaction* transaction,
        [[maybe_unused]] TableDeleteState& deleteState) override {
        throw common::RuntimeException("Cannot delete from parquet-backed node table");
        return false;
    }

    common::row_idx_t getNumTotalRows(const transaction::Transaction* transaction) override;

    const std::string& getParquetFilePath() const { return parquetFilePath; }

    // Note: Cannot override getNumCommittedNodeGroups since it's not virtual in base class
    // Will need a different approach

private:
    std::string parquetFilePath;
    const catalog::NodeTableCatalogEntry* nodeTableCatalogEntry;
    mutable std::unique_ptr<ParquetNodeTableSharedState> sharedState;

    void initializeParquetReader(transaction::Transaction* transaction) const;
    void initParquetScanForRowGroup(transaction::Transaction* transaction,
        ParquetNodeTableScanState& scanState) const;
};

} // namespace storage
} // namespace lbug
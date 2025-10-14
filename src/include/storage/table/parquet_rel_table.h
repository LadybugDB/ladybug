#pragma once

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/types/internal_id_util.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"
#include "storage/table/rel_table.h"
#include "transaction/transaction.h"

namespace lbug {
namespace storage {

struct ParquetRelTableScanState final : RelTableScanState {
    std::unique_ptr<processor::ParquetReaderScanState> parquetScanState;
    // For CSR format: store matching rows for current bound node
    size_t nextRowToProcess = 0;

    // Row group range for morsel-driven parallelism
    uint64_t startRowGroup = 0;
    uint64_t endRowGroup = 0;
    uint64_t currentRowGroup = 0;

    // Per-scan-state readers for thread safety
    std::unique_ptr<processor::ParquetReader> nodeMappingReader;
    std::unique_ptr<processor::ParquetReader> indicesReader;
    std::unique_ptr<processor::ParquetReader> indptrReader;

    ParquetRelTableScanState(MemoryManager& mm, common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : RelTableScanState{mm, nodeIDVector, std::move(outputVectors), std::move(outChunkState)} {
        parquetScanState = std::make_unique<processor::ParquetReaderScanState>();
    }

    void setToTable(const transaction::Transaction* transaction, Table* table_,
        std::vector<common::column_id_t> columnIDs_,
        std::vector<ColumnPredicateSet> columnPredicateSets_,
        common::RelDataDirection direction_) override;
};

class ParquetRelTable final : public RelTable {
public:
    ParquetRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
        common::table_id_t toTableID, const StorageManager* storageManager,
        MemoryManager* memoryManager);

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

    // For parquet-backed tables, we don't support modifications
    void insert([[maybe_unused]] transaction::Transaction* transaction,
        [[maybe_unused]] TableInsertState& insertState) override {
        throw common::RuntimeException("Cannot insert into parquet-backed rel table");
    }
    void update([[maybe_unused]] transaction::Transaction* transaction,
        [[maybe_unused]] TableUpdateState& updateState) override {
        throw common::RuntimeException("Cannot update parquet-backed rel table");
    }
    bool delete_([[maybe_unused]] transaction::Transaction* transaction,
        [[maybe_unused]] TableDeleteState& deleteState) override {
        throw common::RuntimeException("Cannot delete from parquet-backed rel table");
        return false;
    }

    common::row_idx_t getNumTotalRows(const transaction::Transaction* transaction) override;

private:
    catalog::RelGroupCatalogEntry* relGroupEntry; // Store reference to table schema
    std::string nodeMappingFilePath;
    std::string indicesFilePath;
    std::string indptrFilePath;
    mutable std::unique_ptr<processor::ParquetReader> nodeMappingReader;
    mutable std::unique_ptr<processor::ParquetReader> indicesReader;
    mutable std::unique_ptr<processor::ParquetReader> indptrReader;
    mutable std::mutex parquetReaderMutex;
    mutable std::mutex indptrDataMutex;
    mutable std::vector<common::offset_t> indptrData; // Cached indptr data for CSR format
    mutable common::internal_id_map_t<common::offset_t>
        nodeMapping; // Maps node IDs to CSR node IDs
    mutable std::unordered_map<common::offset_t, common::offset_t>
        csrToNodeTableIdMap; // Reverse mapping: CSR node ID to node table ID

    void initializeParquetReaders(transaction::Transaction* transaction) const;
    void initializeIndptrReader(transaction::Transaction* transaction) const;
    void loadIndptrData(transaction::Transaction* transaction) const;
    void loadNodeMappingData(transaction::Transaction* transaction) const;
    bool scanInternalByRowGroups(transaction::Transaction* transaction,
        ParquetRelTableScanState& parquetRelScanState);
    bool scanRowGroupForBoundNodes(transaction::Transaction* transaction,
        ParquetRelTableScanState& parquetRelScanState,
        const std::vector<uint64_t>& rowGroupsToProcess,
        const std::unordered_set<common::offset_t>& boundNodeOffsets);
    common::offset_t findSourceNodeForRow(common::offset_t globalRowIdx) const;
};

} // namespace storage
} // namespace lbug

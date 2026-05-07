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

struct IceDiskRelTableScanState : public RelTableScanState {
private:
    std::mutex mtx;
public:
    std::unique_ptr<processor::ParquetReader> indicesReader;
    std::unique_ptr<processor::ParquetReaderScanState> parquetScanState;
    std::size_t currentRowGroupIdx = 0;

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

    void initializeIndicesReader(transaction::Transaction* transaction);
};

class IceDiskRelTable final : public RelTable {
public:
    IceDiskRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
        common::table_id_t toTableID, const StorageManager* storageManager,
        MemoryManager* memoryManager);
    
    void initializeScanCoordination(transaction::Transaction* transaction);
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

private:
    std::vector<std::size_t> getIndicesRowGroupStartOffsets(const transaction::Transaction* transaction) const;
    std::vector<std::size_t> readIndptrData(transaction::Transaction* transaction) const;
    void copyCachedBoundNodeSelVector(RelTableScanState& relScanState) const;
    bool scanRowGroupForBoundNodes(transaction::Transaction* transaction,
    IceDiskRelTableScanState& iceDiskScanState, const std::vector<std::size_t>& rowGroupsToProcess,
    const std::unordered_set<common::offset_t>& boundNodeOffsets);
    std::size_t findSourceNodeForRow(std::size_t globalRowIdx) const;

private:
    std::string indicesFilePath;
    std::string indptrFilePath;
    const catalog::RelGroupCatalogEntry* relGroupCatalogEntry;
    // Row group start offsets derived from Parquet metadata; stable for the lifetime of the table.
    std::vector<std::size_t> indicesRowGroupStartOffsets;
    // Full CSR indptr array loaded once from disk; stable for the lifetime of the table.
    std::vector<std::size_t> indptrData;
    constexpr static std::size_t scanRowGroupBatchSize = 2048; // Default batch size
};

} // namespace storage
} // namespace lbug

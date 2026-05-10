#pragma once

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/types/internal_id_util.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"
#include "storage/table/columnar_rel_table_base.h"
#include "transaction/transaction.h"

namespace lbug {
namespace storage {

struct IceDiskRelTableScanState final : RelTableScanState {
    std::unique_ptr<processor::ParquetReaderScanState> parquetScanState;

    // Row group range for morsel-driven parallelism
    uint64_t currentRowGroup = 0;
    uint64_t endRowGroup = 0;

    // Per-scan-state readers for thread safety
    std::unique_ptr<processor::ParquetReader> indicesReader;
    std::unique_ptr<processor::ParquetReader> indptrReader;

    IceDiskRelTableScanState(MemoryManager& mm, common::ValueVector* nodeIDVector,
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

class IceDiskRelTable final : public ColumnarRelTableBase {
public:
    IceDiskRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
        common::table_id_t toTableID, const StorageManager* storageManager,
        MemoryManager* memoryManager);

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

protected:
    // Implement ColumnarRelTableBase interface
    std::string getColumnarFormatName() const override { return "icebug-disk"; }
    common::row_idx_t getTotalRowCount(const transaction::Transaction* transaction) const override;

private:
    std::string indicesFilePath;
    std::string indptrFilePath;
    mutable std::unique_ptr<processor::ParquetReader> indicesReader;
    mutable std::unique_ptr<processor::ParquetReader> indptrReader;
    mutable std::mutex parquetReaderMutex;
    mutable std::mutex indptrDataMutex;
    mutable std::vector<common::offset_t> indptrData; // Cached indptr data for CSR format

    void initializeParquetReaders(transaction::Transaction* transaction) const;
    void initializeIndptrReader(transaction::Transaction* transaction) const;
    void loadIndptrData(transaction::Transaction* transaction) const;
    bool scanInternalByRowGroups(transaction::Transaction* transaction,
        IceDiskRelTableScanState& iceDiskScanState);
    bool scanRowGroupForBoundNodes(transaction::Transaction* transaction,
        IceDiskRelTableScanState& iceDiskScanState, const std::vector<uint64_t>& rowGroupsToProcess,
        const std::unordered_map<common::offset_t, common::sel_t>& boundNodeOffsets);
    common::offset_t findSourceNodeForRow(common::offset_t globalRowIdx) const;
};

} // namespace storage
} // namespace lbug

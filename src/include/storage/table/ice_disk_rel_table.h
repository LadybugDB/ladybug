#pragma once

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/types/internal_id_util.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"
#include "storage/table/columnar_rel_table_base.h"
#include "transaction/transaction.h"

namespace lbug {
namespace main {
class ClientContext;
} // namespace main
namespace storage {

struct IceDiskRelTableScanState final : RelTableScanState {
    std::unique_ptr<processor::ParquetReaderScanState> parquetScanState;

    // cached data for the current batch in current row group
    std::unique_ptr<common::DataChunk> cachedBatchData;
    common::offset_t currentBatchStartOffset =
        0; // Global row index of the start of the current batch of the current row group
    common::offset_t currentLocalRowIdx =
        0; // Row index within the current batch of the current row group
    std::unordered_map<common::offset_t, common::sel_t>
        boundNodeOffsets; // Map from bound node offset to selection vector index

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

    void reset(std::unordered_map<common::offset_t, common::sel_t> boundNodeOffsets_) {
        cachedBatchData = nullptr;
        currentBatchStartOffset = 0;
        currentLocalRowIdx = 0;
        boundNodeOffsets = std::move(boundNodeOffsets_);
    }

    void reloadCachedBatchData(transaction::Transaction* transaction);
};

class IceDiskRelTable final : public ColumnarRelTableBase {
public:
    IceDiskRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
        common::table_id_t toTableID, const StorageManager* storageManager,
        MemoryManager* memoryManager, main::ClientContext* context = nullptr);

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
    common::offset_t findSourceNodeForRow(common::offset_t globalRowIdx) const;
};

} // namespace storage
} // namespace lbug

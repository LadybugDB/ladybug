#pragma once

#include <mutex>
#include <vector>

#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/types/internal_id_util.h"
#include "common/types/value/value.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"
#include "storage/table/columnar_node_table_base.h"

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

class ParquetNodeTable final : public ColumnarNodeTableBase {
public:
    ParquetNodeTable(const StorageManager* storageManager,
        const catalog::NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager);

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

    const std::string& getParquetFilePath() const { return parquetFilePath; }

protected:
    // Implement ColumnarNodeTableBase interface
    std::string getColumnarFormatName() const override { return "Parquet"; }
    common::node_group_idx_t getNumBatches(
        const transaction::Transaction* transaction) const override;
    common::row_idx_t getTotalRowCount(const transaction::Transaction* transaction) const override;

private:
    std::string parquetFilePath;

    void initializeParquetReader(transaction::Transaction* transaction) const;
    void initParquetScanForRowGroup(transaction::Transaction* transaction,
        ParquetNodeTableScanState& scanState) const;
};

} // namespace storage
} // namespace lbug
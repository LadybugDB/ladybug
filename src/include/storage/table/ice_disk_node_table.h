#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/types/internal_id_util.h"
#include "common/types/value/value.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"
#include "storage/table/columnar_node_table_base.h"

namespace lbug {
namespace main {
class ClientContext;
} // namespace main
namespace storage {

struct IceDiskNodeTableScanState final : ColumnarNodeTableScanState {
    std::unique_ptr<processor::ParquetReader> parquetReader;
    std::unique_ptr<processor::ParquetReaderScanState> parquetScanState;
    bool dataRead = false;
    std::vector<std::vector<std::unique_ptr<common::Value>>> allData;
    size_t nextRowToDistribute = 0;
    uint64_t lastQueryId = 0; // Track the last query ID to detect new queries

    IceDiskNodeTableScanState([[maybe_unused]] MemoryManager& mm, common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : ColumnarNodeTableScanState{mm, nodeIDVector, std::move(outputVectors),
              std::move(outChunkState)} {
        parquetScanState = std::make_unique<processor::ParquetReaderScanState>();
    }
};

struct IceDiskNodeTableScanSharedState final : ColumnarNodeTableScanSharedState {
private:
    std::mutex mtx;
    common::node_group_idx_t currentBatchIdx = 0;
    common::node_group_idx_t numBatches = 0;

public:
    void reset(common::node_group_idx_t totalBatches) {
        std::lock_guard<std::mutex> lock(mtx);
        currentBatchIdx = 0;
        numBatches = totalBatches;
    }

    bool getNextBatch(common::node_group_idx_t& assignedBatchIdx) {
        std::lock_guard<std::mutex> lock(mtx);
        if (currentBatchIdx < numBatches) {
            assignedBatchIdx = currentBatchIdx++;
            return true;
        }
        return false;
    }

    // TODO : getNextBatch to be replaced with this
    // See: https://github.com/LadybugDB/ladybug/issues/245
    bool getNextMorsel(ColumnarNodeTableScanState*) override { return false; }
};

class IceDiskNodeTable final : public ColumnarNodeTableBase {
public:
    IceDiskNodeTable(const StorageManager* storageManager,
        const catalog::NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager,
        main::ClientContext* context = nullptr);

    void initializeScanCoordination(const transaction::Transaction* transaction) override;

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

    bool isVisible(const transaction::Transaction* transaction,
        common::offset_t offset) const override;
    bool isVisibleNoLock(const transaction::Transaction* transaction,
        common::offset_t offset) const override;

    const std::string& getParquetFilePath() const { return parquetFilePath; }

protected:
    // Implement ColumnarNodeTableBase interface
    std::string getColumnarFormatName() const override { return "icebug-disk"; }
    common::node_group_idx_t getNumBatches(
        const transaction::Transaction* transaction) const override;
    common::row_idx_t getTotalRowCount(const transaction::Transaction* transaction) const override;

private:
    std::string parquetFilePath;
    mutable std::atomic<common::row_idx_t> cachedRowCount{common::INVALID_ROW_IDX};

    void initializeParquetReader(transaction::Transaction* transaction) const;
    void initParquetScanForRowGroup(transaction::Transaction* transaction,
        IceDiskNodeTableScanState& iceDiskScanState) const;
};

} // namespace storage
} // namespace lbug
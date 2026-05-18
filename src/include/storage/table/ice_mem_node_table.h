#pragma once

#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/arrow/arrow.h"
#include "common/cast.h"
#include "common/exception/runtime.h"
#include "function/table/table_function.h"
#include "storage/table/columnar_node_table_base.h"

namespace lbug {
namespace storage {

struct IceMemNodeTableScanState final : ColumnarNodeTableScanState {
    size_t currentBatchIdx = static_cast<size_t>(common::INVALID_NODE_GROUP_IDX);
    size_t currentMorselStartOffset = 0;
    size_t currentMorselEndOffset = 0;

    IceMemNodeTableScanState(MemoryManager& mm, common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : ColumnarNodeTableScanState{mm, nodeIDVector, std::move(outputVectors),
              std::move(outChunkState)} {}
};

struct IceMemNodeTableScanSharedState final : ColumnarNodeTableScanSharedState {
private:
    std::mutex mtx;
    std::vector<size_t> batchSizes;
    common::node_group_idx_t currentBatchIdx = 0;
    size_t currentMorselStartOffset = 0;
    const size_t morselSize;

public:
    IceMemNodeTableScanSharedState(const size_t morselSize)
        : ColumnarNodeTableScanSharedState(), morselSize(morselSize) {}

    void reset(std::vector<size_t> batchSizes) {
        std::lock_guard<std::mutex> lock(mtx);
        this->batchSizes = batchSizes;
        this->currentBatchIdx = 0;
        this->currentMorselStartOffset = 0;
    }

    bool getNextMorsel(ColumnarNodeTableScanState* scanState) override {
        auto* iceMemScanState = common::dynamic_cast_checked<IceMemNodeTableScanState*>(scanState);
        std::lock_guard<std::mutex> lock(mtx);

        while (currentBatchIdx < batchSizes.size()) {
            auto batchLength = batchSizes[currentBatchIdx];

            if (currentMorselStartOffset < batchLength) {
                iceMemScanState->currentBatchIdx = currentBatchIdx;
                iceMemScanState->currentMorselStartOffset = currentMorselStartOffset;
                iceMemScanState->currentMorselEndOffset =
                    std::min(currentMorselStartOffset + morselSize, batchLength);
                this->currentMorselStartOffset = iceMemScanState->currentMorselEndOffset;

                return true;
            }

            this->currentBatchIdx++;
            this->currentMorselStartOffset = 0;
        }

        return false;
    }
};

class IceMemNodeTable final : public ColumnarNodeTableBase {
public:
    IceMemNodeTable(const StorageManager* storageManager,
        const catalog::NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager);

    ~IceMemNodeTable();

    void initializeScanCoordination(const transaction::Transaction* transaction) override;

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

    bool isVisible(const transaction::Transaction* transaction,
        common::offset_t offset) const override;
    bool isVisibleNoLock(const transaction::Transaction* transaction,
        common::offset_t offset) const override;

    common::node_group_idx_t getNumBatches(
        const transaction::Transaction* transaction) const override;

    size_t getNumScanMorsels(const transaction::Transaction* transaction) const;

    const catalog::NodeTableCatalogEntry* getCatalogEntry() const { return nodeTableCatalogEntry; }

protected:
    std::string getColumnarFormatName() const override { return "icebug-memory"; }
    common::row_idx_t getTotalRowCount(const transaction::Transaction* transaction) const override;

private:
    std::vector<int64_t> getOutputToArrowColumnIdx(
        const std::vector<common::column_id_t>& columnIDs) const;

private:
    ArrowSchemaWrapper schema;
    std::vector<ArrowArrayWrapper> arrays;
    std::vector<size_t> batchStartOffsets;
    size_t totalRows;
    std::string arrowId;                           // ID in registry for cleanup
    constexpr static size_t scanMorselSize = 2048; // Default morsel size
};

} // namespace storage
} // namespace lbug

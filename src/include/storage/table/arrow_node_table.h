#pragma once

#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/arrow/arrow.h"
#include "common/exception/runtime.h"
#include "function/table/table_function.h"
#include "storage/table/columnar_node_table_base.h"

namespace lbug {
namespace storage {

struct ArrowNodeTableScanState final : ColumnarNodeTableScanState {
    size_t currentBatchIdx = common::INVALID_NODE_GROUP_IDX;
    size_t currentMorselStartOffset = 0; // Start of current morsel within batch
    size_t currentMorselEndOffset = 0;   // End of current morsel within batch
    std::vector<int64_t> outputToArrowColumnIdx;
    bool initialized = false;
    bool scanCompleted = false;

    ArrowNodeTableScanState(MemoryManager& mm, common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : ColumnarNodeTableScanState{mm, nodeIDVector, std::move(outputVectors),
              std::move(outChunkState)} {}
};

struct ArrowNodeTableScanSharedState final : ColumnarNodeTableScanSharedState {
private:
    std::mutex mtx;
    std::vector<size_t> batchSizes;
    common::node_group_idx_t currentBatchIdx = 0;
    size_t currentMorselStartOffset = 0;
    const size_t morselSize;

public:
    ArrowNodeTableScanSharedState(const size_t morselSize)
        : ColumnarNodeTableScanSharedState(), morselSize(morselSize) {}

    void reset(std::vector<size_t> batchSizes) {
        std::lock_guard<std::mutex> lock(mtx);
        this->batchSizes = batchSizes;
        this->currentBatchIdx = 0;
        this->currentMorselStartOffset = 0;
    }

    bool getNextMorsel(ColumnarNodeTableScanState* scanState) override {
        auto arrowScanState = static_cast<ArrowNodeTableScanState*>(scanState);
        std::lock_guard<std::mutex> lock(mtx);

        while (currentBatchIdx < batchSizes.size()) {
            auto batchLength = batchSizes[currentBatchIdx];

            if (currentMorselStartOffset < batchLength) {
                arrowScanState->currentBatchIdx = currentBatchIdx;
                arrowScanState->currentMorselStartOffset = currentMorselStartOffset;
                arrowScanState->currentMorselEndOffset =
                    std::min(currentMorselStartOffset + morselSize, batchLength);
                this->currentMorselStartOffset = arrowScanState->currentMorselEndOffset;

                return true;
            }

            this->currentBatchIdx++;
            this->currentMorselStartOffset = 0;
        }

        return false;
    }
};

class ArrowNodeTable final : public ColumnarNodeTableBase {
public:
    ArrowNodeTable(const StorageManager* storageManager,
        const catalog::NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager,
        ArrowSchemaWrapper schema, std::vector<ArrowArrayWrapper> arrays, std::string arrowId);

    ~ArrowNodeTable();

    void initializeScanCoordination(const transaction::Transaction* transaction) override;

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

    const ArrowSchemaWrapper& getArrowSchema() const { return schema; }
    const std::vector<ArrowArrayWrapper>& getArrowArrays() const { return arrays; }

    common::node_group_idx_t getNumBatches(
        const transaction::Transaction* transaction) const override;

    size_t getNumScanMorsels(const transaction::Transaction* transaction) const;

protected:
    std::string getColumnarFormatName() const override { return "Arrow"; }
    common::row_idx_t getTotalRowCount(const transaction::Transaction* transaction) const override;

private:
    std::vector<size_t> getBatchSizes(
        [[maybe_unused]] const transaction::Transaction* transaction) const;

    void copyArrowMorselToOutputVectors(const ArrowArrayWrapper& batch,
        const size_t currentMorselStartOffset, const uint64_t numRowsToCopy,
        const std::vector<common::ValueVector*>& outputVectors,
        const std::vector<int64_t>& outputToArrowColumnIdx) const;

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

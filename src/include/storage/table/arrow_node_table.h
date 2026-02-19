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

struct ArrowNodeTableScanState final : NodeTableScanState {
    size_t totalRows = 0;
    size_t currentBatchIdx = 0;
    size_t currentBatchOffset = 0;
    size_t nextGlobalRowOffset = 0;
    std::vector<int64_t> outputToArrowColumnIdx;
    bool initialized = false;
    bool scanCompleted = false;

    ArrowNodeTableScanState([[maybe_unused]] MemoryManager& mm, common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : NodeTableScanState{nodeIDVector, std::move(outputVectors), std::move(outChunkState)} {}
};

class ArrowNodeTable final : public ColumnarNodeTableBase {
public:
    ArrowNodeTable(const StorageManager* storageManager,
        const catalog::NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager,
        ArrowSchemaWrapper schema, std::vector<ArrowArrayWrapper> arrays, std::string arrowId);

    ~ArrowNodeTable();

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

    const ArrowSchemaWrapper& getArrowSchema() const { return schema; }
    const std::vector<ArrowArrayWrapper>& getArrowArrays() const { return arrays; }

    common::node_group_idx_t getNumBatches(
        const transaction::Transaction* transaction) const override;

protected:
    std::string getColumnarFormatName() const override { return "Arrow"; }
    common::row_idx_t getTotalRowCount(const transaction::Transaction* transaction) const override;

private:
    ArrowSchemaWrapper schema;
    std::vector<ArrowArrayWrapper> arrays;
    std::vector<size_t> batchStartOffsets;
    size_t totalRows;
    std::string arrowId; // ID in registry for cleanup
};

} // namespace storage
} // namespace lbug

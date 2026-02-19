#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/arrow/arrow.h"
#include "storage/table/columnar_rel_table_base.h"
#include "storage/table/node_table.h"

namespace lbug {
namespace storage {

struct ArrowRelTableScanState final : RelTableScanState {
    size_t currentBatchIdx = 0;
    size_t currentBatchOffset = 0;
    std::vector<int64_t> outputToArrowColumnIdx;
    std::unordered_map<common::offset_t, common::sel_t> boundNodeOffsetToSelPos;
    std::unique_ptr<common::ValueVector> srcKeyVector;
    std::unique_ptr<common::ValueVector> dstKeyVector;
    // ScanRelTable invokes scan() once before the first initScanState() call for a bound node.
    // Start as completed so this pre-init call returns safely.
    bool scanCompleted = true;

    ArrowRelTableScanState(MemoryManager& mm, common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : RelTableScanState{mm, nodeIDVector, std::move(outputVectors), std::move(outChunkState)} {}

    void setToTable(const transaction::Transaction* transaction, Table* table_,
        std::vector<common::column_id_t> columnIDs_,
        std::vector<ColumnPredicateSet> columnPredicateSets_,
        common::RelDataDirection direction_) override;
};

class ArrowRelTable final : public ColumnarRelTableBase {
public:
    ArrowRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
        common::table_id_t toTableID, const StorageManager* storageManager,
        MemoryManager* memoryManager, const NodeTable* fromNodeTable, const NodeTable* toNodeTable,
        ArrowSchemaWrapper schema, std::vector<ArrowArrayWrapper> arrays, std::string arrowId);
    ~ArrowRelTable();

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

protected:
    std::string getColumnarFormatName() const override { return "Arrow"; }
    common::row_idx_t getTotalRowCount(const transaction::Transaction* transaction) const override;

private:
    int64_t fromColumnIdx = -1;
    int64_t toColumnIdx = -1;
    const NodeTable* fromNodeTable;
    const NodeTable* toNodeTable;
    ArrowSchemaWrapper schema;
    std::vector<ArrowArrayWrapper> arrays;
    std::vector<size_t> batchStartOffsets;
    std::unordered_map<common::column_id_t, int64_t> propertyColumnToArrowColumnIdx;
    size_t totalRows = 0;
    std::string arrowId;
};

} // namespace storage
} // namespace lbug

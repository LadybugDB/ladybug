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

struct IceMemRelTableScanState final : RelTableScanState {
    IceMemRelTableScanState(MemoryManager& mm, common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : RelTableScanState{mm, nodeIDVector, std::move(outputVectors), std::move(outChunkState)} {}

    void setToTable(const transaction::Transaction* transaction, Table* table_,
        std::vector<common::column_id_t> columnIDs_,
        std::vector<ColumnPredicateSet> columnPredicateSets_,
        common::RelDataDirection direction_) override;
};

class IceMemRelTable final : public ColumnarRelTableBase {
public:
    IceMemRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
        common::table_id_t toTableID, const StorageManager* storageManager,
        MemoryManager* memoryManager);
    ~IceMemRelTable();

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

protected:
    std::string getColumnarFormatName() const override { return "icebug-memory"; }
    common::row_idx_t getTotalRowCount(const transaction::Transaction* transaction) const override;

private:
    common::offset_t findSourceNodeForRow(uint64_t globalRowOffset) const;

    ArrowSchemaWrapper indicesSchema;
    ArrowSchemaWrapper indptrSchema;
    std::vector<ArrowArrayWrapper> indices;
    std::vector<ArrowArrayWrapper> indptr;
    std::vector<size_t> batchStartOffsets;                                           // of indices
    std::unordered_map<common::column_id_t, int64_t> propertyColumnToArrowColumnIdx; // of indices
    size_t totalIndicesRows = 0;
};

} // namespace storage
} // namespace lbug

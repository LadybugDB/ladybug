#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/arrow/arrow.h"
#include "storage/table/arrow_table_support.h"
#include "storage/table/columnar_rel_table_base.h"
#include "storage/table/node_table.h"

namespace lbug {
namespace storage {

struct ArrowRelTableScanState final : RelTableScanState {
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
        ArrowRelTableLayout layout, ArrowSchemaWrapper schema,
        std::vector<ArrowArrayWrapper> arrays, ArrowSchemaWrapper indptrSchema,
        std::vector<ArrowArrayWrapper> indptrArrays, std::string arrowId,
        std::string dstColumnName = "to");
    ~ArrowRelTable();

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

protected:
    std::string getColumnarFormatName() const override { return "Arrow"; }
    common::row_idx_t getTotalRowCount(const transaction::Transaction* transaction) const override;
    common::row_idx_t getActiveBoundNodeCount(const transaction::Transaction* transaction,
        common::RelDataDirection direction) const override;
    std::vector<std::pair<common::offset_t, common::row_idx_t>> getAllDegreeEntries(
        const transaction::Transaction* transaction,
        common::RelDataDirection direction) const override;
    std::vector<std::pair<common::offset_t, common::row_idx_t>> getTopKDegreeEntries(
        const transaction::Transaction* transaction, common::RelDataDirection direction,
        common::idx_t k) const override;

private:
    int64_t fromColumnIdx = -1;
    int64_t toColumnIdx = -1;
    int64_t csrNbrColumnIdx = -1;
    int64_t csrIndptrColumnIdx = 0;
    std::vector<int64_t> getOutputToArrowColumnIdx(
        const std::vector<common::column_id_t>& columnIDs) const;
    bool scanFlat(transaction::Transaction* transaction, TableScanState& scanState);
    bool scanCSR(TableScanState& scanState);
    bool readCSRValue(common::ValueVector& outputVector, common::offset_t relOffset,
        uint64_t dstOffset) const;
    bool readIndptr(common::offset_t srcOffset, common::offset_t& result) const;
    common::offset_t findCSRSourceOffset(common::offset_t relOffset) const;
    bool readArrowValueAtOffset(const ArrowSchemaWrapper& arrowSchema,
        const std::vector<ArrowArrayWrapper>& arrowArrays, const std::vector<size_t>& startOffsets,
        int64_t columnIdx, common::offset_t rowOffset, common::ValueVector& outputVector,
        uint64_t dstOffset) const;

    const NodeTable* fromNodeTable;
    const NodeTable* toNodeTable;
    ArrowRelTableLayout layout;
    ArrowSchemaWrapper schema;
    std::vector<ArrowArrayWrapper> arrays;
    std::vector<size_t> batchStartOffsets;
    ArrowSchemaWrapper indptrSchema;
    std::vector<ArrowArrayWrapper> indptrArrays;
    std::vector<size_t> indptrBatchStartOffsets;
    std::unordered_map<common::column_id_t, int64_t> propertyColumnToArrowColumnIdx;
    size_t totalRows = 0;
    size_t totalIndptrRows = 0;
    std::string arrowId;
};

} // namespace storage
} // namespace lbug

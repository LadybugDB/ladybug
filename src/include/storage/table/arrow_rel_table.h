#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/arrow/arrow.h"
#include "storage/table/arrow_csr_rel_data.h"
#include "storage/table/columnar_rel_table_base.h"
#include "storage/table/node_table.h"

namespace lbug {
namespace storage {

// Whether a rel table stores edges as a flat edge list (from, to, props) or as CSR adjacency.
enum class ArrowRelLayout { EdgeList, Csr };

// Scan cursor for CSR scans: tracks the current bound node and next edge to emit.
struct ArrowCsrCursor {
    size_t boundNodeIdx = 0; // index into cachedBoundNodeSelVector
    uint64_t edgeIdx = 0;    // next global edge index to emit
    uint64_t edgeEnd = 0;    // exclusive end for current bound node
};

struct ArrowRelTableScanState final : RelTableScanState {
    // Present for CSR FWD scans and CSR BWD scans when bwd adjacency is available.
    std::optional<ArrowCsrCursor> csrCursor;

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
    // Zero-copy view over Arrow indptr batches: reads UINT64 values directly from
    // the Arrow buffers without an extra flat copy.
    struct IndptrView {
        const std::vector<ArrowArrayWrapper>& batches;
        const std::vector<size_t>& batchOffsets;
        size_t totalSize;

        bool empty() const { return totalSize == 0; }
        size_t size() const { return totalSize; }

        uint64_t operator[](size_t i) const {
            // Binary-search batchOffsets to find which batch holds index i.
            auto it = std::upper_bound(batchOffsets.begin(), batchOffsets.end(), i);
            if (it != batchOffsets.begin()) {
                --it;
            }
            const size_t batchIdx = static_cast<size_t>(it - batchOffsets.begin());
            const size_t localIdx = i - *it;
            const auto* col = batches[batchIdx].children[0];
            return static_cast<const uint64_t*>(col->buffers[1])[col->offset + localIdx];
        }
    };

    // EdgeList constructor: edges stored as flat (from, to, props...) rows.
    ArrowRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
        common::table_id_t toTableID, const StorageManager* storageManager,
        MemoryManager* memoryManager, const NodeTable* fromNodeTable, const NodeTable* toNodeTable,
        ArrowSchemaWrapper schema, std::vector<ArrowArrayWrapper> arrays, std::string arrowId);

    // CSR constructor: edges stored as pre-built CSR adjacency arrays.
    // Src/Dst node tables MUST be ArrowNodeTable; throws RuntimeException otherwise.
    ArrowRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
        common::table_id_t toTableID, const StorageManager* storageManager,
        MemoryManager* memoryManager, const NodeTable* fromNodeTable, const NodeTable* toNodeTable,
        ArrowCsrRelData csrData, std::string arrowId);

    ~ArrowRelTable();

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

protected:
    std::string getColumnarFormatName() const override { return "Arrow"; }
    common::row_idx_t getTotalRowCount(const transaction::Transaction* transaction) const override;

private:
    // ── EdgeList helpers ──────────────────────────────────────────────────
    void initEdgeListScanState(RelTableScanState& relScanState) const;

    bool readEdgeEndpoints(const transaction::Transaction* transaction,
        const ArrowArrayWrapper& batch, size_t rowInBatch, RelTableScanState& relScanState,
        common::offset_t& srcOffset, common::offset_t& dstOffset) const;

    void writeEdgeListRow(const ArrowArrayWrapper& batch, size_t rowInBatch, size_t globalEdgeIdx,
        common::offset_t nbrOffset, common::table_id_t nbrTableID, uint32_t outputCount,
        const std::vector<int64_t>& outputColumnIndices, TableScanState& scanState) const;

    bool scanEdgeList(const transaction::Transaction* transaction, TableScanState& scanState);

    // ── CSR helpers ───────────────────────────────────────────────────────
    void initCsrScanState(RelTableScanState& relScanState) const;

    bool advanceCursorToNextBound(ArrowCsrCursor& cursor, const RelTableScanState& relScanState,
        const IndptrView& indptr) const;

    void setupNodeEdgeRange(ArrowCsrCursor& cursor, const RelTableScanState& relScanState,
        const IndptrView& indptr) const;

    void writeCsrRow(uint64_t globalEdgeIdx, common::offset_t nbrOffset,
        common::table_id_t nbrTableID, uint32_t outputCount,
        const std::vector<int64_t>& outputColumnIndices, TableScanState& scanState) const;

    std::pair<size_t, size_t> findBatch(uint64_t edgeIdx,
        const std::vector<size_t>& batchOffsets) const;

    uint64_t readNeighbourOffset(const ArrowSchema* childSchema, const ArrowArrayWrapper& batch,
        size_t row, common::ValueVector& scratchVec) const;

    // Find the source node index for a given global edge index via binary search over indptr.
    static uint64_t findSourceNodeForRow(uint64_t edgeIdx, const IndptrView& indptr);

    bool scanCsrWithCursor(TableScanState& scanState, const std::vector<ArrowArrayWrapper>& indices,
        const ArrowSchemaWrapper& indicesSchema, const std::vector<size_t>& indexBatchOffsets,
        const IndptrView& indptr, common::table_id_t nbrTableID);
    bool scanCsrBackwardFallback(const transaction::Transaction* transaction,
        TableScanState& scanState);
    bool scanCsr(const transaction::Transaction* transaction, TableScanState& scanState);

    // ── Common helpers ────────────────────────────────────────────────────
    std::vector<int64_t> getOutputColumnIndices(
        const std::vector<common::column_id_t>& columnIDs) const;

    // ── EdgeList data ─────────────────────────────────────────────────────
    int64_t fromColumnIdx = -1;
    int64_t toColumnIdx = -1;
    const NodeTable* fromNodeTable = nullptr;
    const NodeTable* toNodeTable = nullptr;
    ArrowSchemaWrapper edgeListSchema;
    std::vector<ArrowArrayWrapper> edgeListArrays;
    std::vector<size_t> edgeListBatchOffsets;
    std::string arrowId;

    // ── CSR data ──────────────────────────────────────────────────────────
    ArrowSchemaWrapper fwdIndicesSchema;
    std::vector<ArrowArrayWrapper> fwdIndices;
    std::vector<size_t> fwdBatchOffsets;
    std::vector<ArrowArrayWrapper> fwdIndptr;
    std::vector<size_t> fwdIndptrBatchOffsets;
    size_t fwdIndptrTotalEntries = 0;

    bool hasBwd = false;
    ArrowSchemaWrapper bwdIndicesSchema;
    std::vector<ArrowArrayWrapper> bwdIndices;
    std::vector<size_t> bwdBatchOffsets;
    std::vector<ArrowArrayWrapper> bwdIndptr;
    std::vector<size_t> bwdIndptrBatchOffsets;
    size_t bwdIndptrTotalEntries = 0;

    // ── Shared ────────────────────────────────────────────────────────────
    ArrowRelLayout layout = ArrowRelLayout::EdgeList;
    std::unordered_map<common::column_id_t, int64_t> propertyColumnToArrowColumnIdx;
    size_t totalRows = 0;
};

} // namespace storage
} // namespace lbug

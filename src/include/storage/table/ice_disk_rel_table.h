#pragma once

#include <cstdint>
#include <optional>

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/runtime.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"
#include "storage/table/rel_table.h"

namespace lbug {
namespace common {
class VirtualFileSystem;
} // namespace common
namespace main {
class ClientContext;
} // namespace main

namespace storage {

class IceDiskRelTable;

// The scan is reinitialized to the relevant row groups for each bound node. scanBatch is a reusable read buffer; it carries
// no positional state. High-degree nodes are handled by resuming across multiple calls.
struct IceDiskRelTableScanState : public RelTableScanState {
    std::unique_ptr<processor::ParquetReader> indicesReader;         // null until first use
    std::unique_ptr<processor::ParquetReaderScanState> indicesScanState;
    std::unique_ptr<common::DataChunk> scanBatch; // reusable read buffer, lazily allocated

    // Resume state for the currently active bound node.
    // activeEdgeEnd == 0 means no node is active (start fresh from the next bound node).
    uint64_t activeEdgePos = 0;         // global edge row to resume from
    uint64_t activeEdgeEnd = 0;         // exclusive end of the active node's edge range
    common::sel_t    activeSelPos    = 0; // sel-vector position of the active bound node
    common::offset_t activeNodeOffset = 0; // node offset of the active bound node (BWD filter)

    IceDiskRelTableScanState(MemoryManager& mm, common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : RelTableScanState{mm, nodeIDVector, std::move(outputVectors), std::move(outChunkState)},
          indicesScanState{std::make_unique<processor::ParquetReaderScanState>()} {}

    void setToTable(const transaction::Transaction* transaction, Table* table_,
        std::vector<common::column_id_t> columnIDs_,
        std::vector<ColumnPredicateSet> columnPredicateSets_ = {},
        common::RelDataDirection direction_ = common::RelDataDirection::FWD) override;
};

class IceDiskRelTable final : public RelTable {
public:
    IceDiskRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
        common::table_id_t toTableID, const StorageManager* storageManager,
        MemoryManager* memoryManager);

    void initializeScanCoordination(transaction::Transaction* transaction);
    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

    void insert(transaction::Transaction*, TableInsertState&) override {
        throw common::RuntimeException("Cannot insert into icebug-disk-backed rel table");
    }
    void update(transaction::Transaction*, TableUpdateState&) override {
        throw common::RuntimeException("Cannot update icebug-disk-backed rel table");
    }
    bool delete_(transaction::Transaction*, TableDeleteState&) override {
        throw common::RuntimeException("Cannot delete from icebug-disk-backed rel table");
    }

    common::row_idx_t getNumTotalRows(const transaction::Transaction* transaction) override;

    const std::string& getIndicesFilePath() const { return indicesFilePath; }
    const std::string& getIndptrFilePath() const { return indptrFilePath; }
    const catalog::RelGroupCatalogEntry* getRelGroupCatalogEntry() const { return relGroupCatalogEntry; }

private:
    // Lazy-open the indices parquet reader and allocate the reusable scan batch.
    void initIndicesReaderIfNeeded(IceDiskRelTableScanState& iceState,
        main::ClientContext* context, common::VirtualFileSystem* vfs,
        MemoryManager* memMgr) const;

    // Compute the CSR edge range for a node. Returns nullopt when the node has no edges.
    struct EdgeRange { uint64_t start; uint64_t end; };
    std::optional<EdgeRange> getEdgeRange(common::offset_t nodeOffset, bool isFwd) const;

    // Find row groups covering [range.start, range.end), read up to DEFAULT_VECTOR_CAPACITY
    // edges starting at range.start. Returns {count, nextEdgePos} where nextEdgePos == range.end
    // means the node is fully scanned; otherwise resume from nextEdgePos next call.
    struct EdgeScanProgress {
        uint64_t count;        // edges written to output vectors
        uint64_t nextEdgePos;  // global edge row to resume from next call
    };
    EdgeScanProgress collectNodeEdges(RelTableScanState& state, IceDiskRelTableScanState& iceState,
        EdgeRange range, common::offset_t nodeOffset, bool isFwd,
        common::table_id_t nbrTableID, common::VirtualFileSystem* vfs) const;

    void loadIndptrData(transaction::Transaction* transaction);
    void loadIndicesMetadata(transaction::Transaction* transaction);
    void copyCachedBoundNodeSelVector(RelTableScanState& relScanState) const;
    std::size_t findSourceNodeForRow(std::size_t globalRowIdx) const;

private:
    std::string indicesFilePath;
    std::string indptrFilePath;
    const catalog::RelGroupCatalogEntry* relGroupCatalogEntry;
    // CSR indptr: element i = start of node i's edges. Size = numNodes + 1.
    std::vector<std::size_t> indptrData;
    std::vector<std::size_t> indicesRGStarts;
};

} // namespace storage
} // namespace lbug

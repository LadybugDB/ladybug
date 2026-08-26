#pragma once

#include "common/enums/column_evaluate_type.h"
#include "common/partition_routing.h"
#include "common/partition_routing_hook.h"
#include "common/types/types.h"
#include "expression_evaluator/expression_evaluator.h"
#include "processor/operator/persistent/batch_insert.h"
#include "processor/operator/persistent/index_builder.h"
#include "processor/partition_routing.h"
#include "storage/stats/table_stats.h"
#include "storage/table/chunked_node_group.h"

namespace lbug {
namespace storage {
class ColumnChunkData;
class MemoryManager;
class NodeTable;
} // namespace storage
namespace transaction {
class Transaction;
} // namespace transaction

namespace processor {
struct ExecutionContext;

struct NoIndexPKValidator {
    virtual ~NoIndexPKValidator() = default;
    virtual void validate(const storage::ColumnChunkData& pkChunk, common::offset_t startOffset,
        common::length_t numValues) = 0;
    // Called once after all chunks have been validated. Implementations that defer cross-chunk
    // duplicate detection (e.g. by spilling sorted runs to disk and merging) must report any
    // duplicate primary key here by throwing.
    virtual void finalize() {}
};

struct NodeBatchInsertPrintInfo final : OPPrintInfo {
    std::string tableName;

    explicit NodeBatchInsertPrintInfo(std::string tableName) : tableName(std::move(tableName)) {}

    std::string toString() const override;

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::unique_ptr<NodeBatchInsertPrintInfo>(new NodeBatchInsertPrintInfo(*this));
    }

private:
    NodeBatchInsertPrintInfo(const NodeBatchInsertPrintInfo& other)
        : OPPrintInfo(other), tableName(other.tableName) {}
};

struct NodeBatchInsertInfo final : BatchInsertInfo {
    evaluator::evaluator_vector_t columnEvaluators;
    std::vector<common::ColumnEvaluateType> evaluateTypes;
    bool skipDuplicatePK;
    // Set when copying into a partitioned parent. Each row is routed into the partition subgraph
    // whose index matches the row's partition-key value.
    std::optional<common::NodePartitionWriteInfo> partitionInfo;

    NodeBatchInsertInfo(std::string tableName, std::vector<common::LogicalType> warningColumnTypes,
        std::vector<std::unique_ptr<evaluator::ExpressionEvaluator>> columnEvaluators,
        std::vector<common::ColumnEvaluateType> evaluateTypes, bool skipDuplicatePK,
        std::optional<common::NodePartitionWriteInfo> partitionInfo = std::nullopt)
        : BatchInsertInfo{std::move(tableName), std::move(warningColumnTypes)},
          columnEvaluators{std::move(columnEvaluators)}, evaluateTypes{std::move(evaluateTypes)},
          skipDuplicatePK{skipDuplicatePK}, partitionInfo{std::move(partitionInfo)} {}

    NodeBatchInsertInfo(const NodeBatchInsertInfo& other)
        : BatchInsertInfo{other}, columnEvaluators{copyVector(other.columnEvaluators)},
          evaluateTypes{other.evaluateTypes}, skipDuplicatePK{other.skipDuplicatePK},
          partitionInfo{other.partitionInfo} {}

    std::unique_ptr<BatchInsertInfo> copy() const override {
        return std::make_unique<NodeBatchInsertInfo>(*this);
    }
};

// Per-partition write target. A non-partitioned COPY has exactly one target; a partitioned COPY
// has one target per partition subgraph. Each target carries its own index/PK state and its own
// shared (cross-worker) leftover node group.
struct NodeBatchInsertTarget {
    storage::NodeTable* table = nullptr;
    // Optimistic allocator scoped to the file that owns `table` (main file or a per-partition
    // child file). Set during shared-state init; owned by the transaction's LocalStorage.
    storage::PageAllocator* optimisticAllocator = nullptr;
    std::optional<IndexBuilder> globalIndexBuilder;
    std::unique_ptr<NoIndexPKValidator> noIndexPKValidator;
    bool usePrimaryKeyIndexCommitInsert = false;
    std::unique_ptr<storage::InMemChunkedNodeGroup> sharedNodeGroup;
};

struct NodeBatchInsertSharedState final : BatchInsertSharedState {
    // Primary key info (identical across partition subgraphs of the same parent).
    common::column_id_t pkColumnID;
    common::LogicalType pkType;
    bool skipDuplicatePK;

    function::TableFuncSharedState* tableFuncSharedState;

    std::vector<common::column_id_t> mainDataColumns;

    // One write target per partition subgraph (or exactly one for a non-partitioned table).
    // Targets whose storage is routed remotely (see common/partition_routing_hook.h) carry a
    // null table pointer; their aligned entries below describe how to route rows through the
    // hooks instead.
    // LIST parents append to this vector as new partitions are discovered mid-COPY (under the
    // router lock); workers mirror the growth in their own `NodeBatchInsertLocalState`.
    std::vector<NodeBatchInsertTarget> targets;
    // Aligned with `targets`; meaningful only when the write goes into a partitioned parent.
    std::vector<common::PartitionRef> partitionRefs;
    std::vector<common::PartitionHandle> partitionHandles;
    // Index of the partition-key column among the evaluated column vectors; INVALID when the
    // write is not into a partitioned parent.
    common::column_id_t partitionKeyColumnIdx = common::INVALID_COLUMN_ID;
    // Set only for LIST-partitioned parents: creates partitions on first sight of a new key
    // value and serializes discovery across COPY workers.
    std::unique_ptr<ListPartitionRouter> listRouter;

    std::shared_ptr<DuplicatePKSkipResult> duplicatePKSkipResult;

    explicit NodeBatchInsertSharedState(std::shared_ptr<FactorizedTable> fTable)
        : BatchInsertSharedState{std::move(fTable)}, pkColumnID{0}, skipDuplicatePK{false},
          tableFuncSharedState{nullptr},
          duplicatePKSkipResult{std::make_shared<DuplicatePKSkipResult>()} {}

    void initTargetPKIndex(const ExecutionContext* context, NodeBatchInsertTarget& target);
};

struct NodeBatchInsertLocalTarget {
    std::unique_ptr<storage::InMemChunkedNodeGroup> chunkedGroup;
    std::optional<IndexBuilder> localIndexBuilder;
    std::optional<NodeBatchInsertErrorHandler> errorHandler;
};

struct NodeBatchInsertLocalState final : BatchInsertLocalState {
    std::vector<NodeBatchInsertLocalTarget> targets;
    DuplicatePKSkipResult duplicatePKSkipResult;

    std::shared_ptr<common::DataChunkState> columnState;
    std::vector<common::ValueVector*> columnVectors;

    // Scratch buffer reused to hold the partition index of every row in the current chunk.
    std::vector<uint64_t> partitionIdxes;

    // Producer tokens for each target's local index builder. LIST COPYs grow this alongside
    // `targets` when a new partition is created mid-copy, so its builder joins the producer
    // count like any pre-existing partition's.
    std::vector<std::optional<ProducerToken>> indexProducerTokens;

    // Per-operator table stats for the non-partitioned (single target) path only. Partitioned
    // COPYs skip per-partition stats collection for now (stats are advisory, not required for
    // correctness).
    std::optional<storage::TableStats> stats;

    NodeBatchInsertLocalState() = default;
};

class NodeBatchInsert final : public BatchInsert {
public:
    NodeBatchInsert(std::unique_ptr<BatchInsertInfo> info,
        std::shared_ptr<BatchInsertSharedState> sharedState,
        std::unique_ptr<PhysicalOperator> child, physical_op_id id,
        std::unique_ptr<OPPrintInfo> printInfo)
        : BatchInsert{std::move(info), std::move(sharedState), id, std::move(printInfo)} {
        children.push_back(std::move(child));
    }

    void initGlobalStateInternal(ExecutionContext* context) override;

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;

    void executeInternal(ExecutionContext* context) override;

    void finalize(ExecutionContext* context) override;
    void finalizeInternal(ExecutionContext* context) override;

    std::unique_ptr<PhysicalOperator> copy() override {
        return std::make_unique<NodeBatchInsert>(info->copy(), sharedState, children[0]->copy(), id,
            printInfo->copy());
    }

    // The node group will be reset so that the only values remaining are the ones which were
    // not written.
    void writeAndResetNodeGroup(transaction::Transaction* transaction, common::idx_t targetIdx,
        std::unique_ptr<storage::InMemChunkedNodeGroup>& nodeGroup,
        std::optional<IndexBuilder>& indexBuilder, storage::MemoryManager* mm,
        storage::PageAllocator& pageAllocator) const;

private:
    void evaluateExpressions(uint64_t numTuples) const;
    void appendIncompleteNodeGroup(transaction::Transaction* transaction, common::idx_t targetIdx,
        std::unique_ptr<storage::InMemChunkedNodeGroup> localNodeGroup,
        std::optional<IndexBuilder>& indexBuilder, storage::MemoryManager* mm) const;
    void clearToIndex(storage::MemoryManager* mm,
        std::unique_ptr<storage::InMemChunkedNodeGroup>& nodeGroup,
        common::offset_t startIndexInGroup) const;

    void copyToNodeGroup(transaction::Transaction* transaction, storage::MemoryManager* mm,
        ExecutionContext* context) const;

    // LIST-only: appends shared + local targets for partitions created mid-copy. `freshTables`
    // maps the ordinal of every newly discovered partition to its table. Called under the router
    // lock.
    void growListTargets(ExecutionContext* context, NodeBatchInsertSharedState* nodeSharedState,
        NodeBatchInsertLocalState* nodeLocalState,
        const std::unordered_map<uint64_t, storage::NodeTable*>& freshTables) const;

    NodeBatchInsertErrorHandler createErrorHandler(ExecutionContext* context,
        storage::NodeTable* nodeTable, DuplicatePKSkipResult* duplicatePKSkipResult) const;

    void writeAndResetNodeGroup(transaction::Transaction* transaction, common::idx_t targetIdx,
        std::unique_ptr<storage::InMemChunkedNodeGroup>& nodeGroup,
        std::optional<IndexBuilder>& indexBuilder, storage::MemoryManager* mm,
        NodeBatchInsertErrorHandler& errorHandler, storage::PageAllocator& pageAllocator) const;
};

} // namespace processor
} // namespace lbug
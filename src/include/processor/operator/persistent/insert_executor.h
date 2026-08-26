#pragma once

#include "common/enums/conflict_action.h"
#include "common/partition_routing.h"
#include "common/partition_routing_hook.h"
#include "expression_evaluator/expression_evaluator.h"
#include "processor/execution_context.h"
#include "processor/partition_routing.h"
#include "storage/table/node_table.h"
#include "storage/table/rel_table.h"

namespace lbug {
namespace processor {

// Operator level info
struct NodeInsertInfo {
    DataPos nodeIDPos;
    // Column vector pos is invalid if it doesn't need to be projected.
    std::vector<DataPos> columnsPos;
    common::ConflictAction conflictAction;

    common::ValueVector* nodeIDVector = nullptr;
    std::vector<common::ValueVector*> columnVectors;

    NodeInsertInfo(DataPos nodeIDPos, std::vector<DataPos> columnsPos,
        common::ConflictAction conflictAction)
        : nodeIDPos{nodeIDPos}, columnsPos{std::move(columnsPos)}, conflictAction{conflictAction} {}
    EXPLICIT_COPY_DEFAULT_MOVE(NodeInsertInfo);

    void init(const ResultSet& resultSet);

    void updateNodeID(common::nodeID_t nodeID) const;
    common::nodeID_t getNodeID() const;

private:
    NodeInsertInfo(const NodeInsertInfo& other)
        : nodeIDPos{other.nodeIDPos}, columnsPos{other.columnsPos},
          conflictAction{other.conflictAction} {}
};

// Table level info
struct NodeTableInsertInfo {
    storage::NodeTable* table;
    evaluator::evaluator_vector_t columnDataEvaluators;

    common::ValueVector* pkVector;
    std::vector<common::ValueVector*> columnDataVectors;
    std::vector<common::column_id_t> columnIDs;

    // When writing into a partitioned parent, `table` is the first partition subgraph (used to
    // derive the PK vector position) and `partitionTables` holds every partition subgraph in
    // partition order. `partitionKeyColumnID` indexes the partition-key column among
    // columnDataVectors. Empty `partitionTables` means a plain single-table write.
    //
    // Partition subgraphs whose storage is routed remotely (see
    // common/partition_routing_hook.h) carry a null table pointer; their entries in
    // `partitionChildIDs` / `partitionRefs` / `partitionHandles` remain valid and describe
    // how to route rows through the hooks instead.
    // LIST parents route through `listRouter` instead: their partition set grows dynamically, so
    // targets cannot be fixed at plan time. `parentTableID` lets the executor build the router
    // lazily (clones of the insert info start with a null router).
    std::vector<storage::NodeTable*> partitionTables;
    std::vector<common::table_id_t> partitionChildIDs;
    std::vector<common::PartitionRef> partitionRefs;
    std::vector<common::PartitionHandle> partitionHandles;
    common::column_id_t partitionKeyColumnID = common::INVALID_COLUMN_ID;
    common::PartitionMethod partitionMethod = common::PartitionMethod::HASH;
    common::table_id_t parentTableID = common::INVALID_TABLE_ID;
    std::unique_ptr<ListPartitionRouter> listRouter;

    NodeTableInsertInfo(storage::NodeTable* table,
        evaluator::evaluator_vector_t columnDataEvaluators)
        : table{table}, columnDataEvaluators{std::move(columnDataEvaluators)}, pkVector{nullptr} {}
    EXPLICIT_COPY_DEFAULT_MOVE(NodeTableInsertInfo);

    void init(const ResultSet& resultSet, main::ClientContext* context);

private:
    NodeTableInsertInfo(const NodeTableInsertInfo& other)
        : table{other.table}, columnDataEvaluators{copyVector(other.columnDataEvaluators)},
          pkVector{nullptr}, columnDataVectors{other.columnDataVectors}, columnIDs{other.columnIDs},
          partitionTables{other.partitionTables}, partitionChildIDs{other.partitionChildIDs},
          partitionRefs{other.partitionRefs}, partitionHandles{other.partitionHandles},
          partitionKeyColumnID{other.partitionKeyColumnID}, partitionMethod{other.partitionMethod},
          parentTableID{other.parentTableID} {}
};

class NodeInsertExecutor {
public:
    NodeInsertExecutor(NodeInsertInfo info, NodeTableInsertInfo tableInfo)
        : info{std::move(info)}, tableInfo{std::move(tableInfo)} {}
    EXPLICIT_COPY_DEFAULT_MOVE(NodeInsertExecutor);

    void init(ResultSet* resultSet, const ExecutionContext* context);

    void setNodeIDVectorToNonNull() const;
    common::nodeID_t insert(main::ClientContext* context);

    // For MERGE, we might need to skip the insert for duplicate input. But still, we need to write
    // the output vector for later usage.
    void skipInsert() const;
    void skipInsert(common::nodeID_t nodeID, main::ClientContext* context) const;

private:
    NodeInsertExecutor(const NodeInsertExecutor& other)
        : info{other.info.copy()}, tableInfo{other.tableInfo.copy()} {}

    bool checkConflict(const transaction::Transaction* transaction,
        storage::NodeTable* table) const;
    // Computes the partition subgraph index for the current (already evaluated) partition-key
    // value.
    uint64_t currentPartitionIndex() const;
    // Resolves the partition subgraph for the current (already evaluated) partition-key value.
    // LIST parents resolve (and, on first sight of a value, create) their partition through the
    // router, so this can grow the parent's partition set. Returns nullptr when that partition
    // is routed remotely through the hooks; callers must branch on that.
    storage::NodeTable* resolveTargetTable(main::ClientContext* context);
    // Ships the current row to the routing wrapper for remote partition `index`.
    common::nodeID_t insertRemotely(uint64_t index, transaction::Transaction* transaction) const;
    storage::NodeTable* resolveTableForNodeID(common::nodeID_t nodeID,
        main::ClientContext* context) const;

private:
    NodeInsertInfo info;
    NodeTableInsertInfo tableInfo;
};

struct RelInsertInfo {
    DataPos srcNodeIDPos;
    DataPos dstNodeIDPos;
    std::vector<DataPos> columnsPos;

    common::ValueVector* srcNodeIDVector;
    common::ValueVector* dstNodeIDVector;
    std::vector<common::ValueVector*> columnVectors;

    RelInsertInfo(DataPos srcNodeIDPos, DataPos dstNodeIDPos, std::vector<DataPos> columnsPos)
        : srcNodeIDPos{srcNodeIDPos}, dstNodeIDPos{dstNodeIDPos}, columnsPos{std::move(columnsPos)},
          srcNodeIDVector{nullptr}, dstNodeIDVector{nullptr} {}
    EXPLICIT_COPY_DEFAULT_MOVE(RelInsertInfo);

    void init(const ResultSet& resultSet);

private:
    RelInsertInfo(const RelInsertInfo& other)
        : srcNodeIDPos{other.srcNodeIDPos}, dstNodeIDPos{other.dstNodeIDPos},
          columnsPos{other.columnsPos}, srcNodeIDVector{nullptr}, dstNodeIDVector{nullptr} {}
};

struct RelTableInsertInfo {
    storage::RelTable* table;
    evaluator::evaluator_vector_t columnDataEvaluators;

    std::vector<common::ValueVector*> columnDataVectors;

    RelTableInsertInfo(storage::RelTable* table, evaluator::evaluator_vector_t evaluators)
        : table{table}, columnDataEvaluators{std::move(evaluators)} {}
    EXPLICIT_COPY_DEFAULT_MOVE(RelTableInsertInfo);

    void init(const ResultSet& resultSet, main::ClientContext* context);
    common::internalID_t getRelID() const;

private:
    RelTableInsertInfo(const RelTableInsertInfo& other)
        : table{other.table}, columnDataEvaluators(copyVector(other.columnDataEvaluators)) {}
};

class RelInsertExecutor {
public:
    RelInsertExecutor(RelInsertInfo info, RelTableInsertInfo tableInfo)
        : info{std::move(info)}, tableInfo{std::move(tableInfo)} {}
    EXPLICIT_COPY_DEFAULT_MOVE(RelInsertExecutor);

    void init(ResultSet* resultSet, const ExecutionContext* context);

    common::internalID_t insert(main::ClientContext* context);

    // See comment in NodeInsertExecutor.
    void skipInsert() const;

private:
    RelInsertExecutor(const RelInsertExecutor& other)
        : info{other.info.copy()}, tableInfo{other.tableInfo.copy()} {}

private:
    RelInsertInfo info;
    RelTableInsertInfo tableInfo;
};

} // namespace processor
} // namespace lbug

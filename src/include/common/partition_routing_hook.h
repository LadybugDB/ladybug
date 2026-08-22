#pragma once

#include <functional>

#include "common/api.h"
#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include <span>

namespace lbug {
namespace function {
struct TableFunction;
struct TableFuncBindData;
} // namespace function
namespace transaction {
class Transaction;
} // namespace transaction

namespace common {

// ---------------------------------------------------------------------------
// Partition routing hooks
//
// Partitioned node tables keep their rows in partition subgraphs (`<parent>_p<i>`
// node tables). In a distributed deployment a partition may live on a remote
// host. These hooks let a distributed wrapper intercept every subgraph access
// and route it remotely, while ladybugdb itself stays embedded and knows
// nothing about hosts, transport, or serialization.
//
// Contract:
//   * The engine owns the partition function (hash/range over the key column)
//     and the full catalog metadata (parent/child table IDs, schemas) for every
//     partition, including remote ones. A wrapper therefore never decides
//     *which* partition a row belongs to - only *where* it lives.
//   * `PartitionHandle` is opaque: the engine receives it from `locate()` and
//     hands it back verbatim afterwards. The wrapper interprets it (e.g. as a
//     connection or host descriptor).
//   * Every member is optional; NULL means "handle locally", which is the
//     default and preserves the embedded behavior bit-for-bit.
//   * Hooks must be installed before the first Database is opened (recovery,
//     checkpointing and query planning all consult them). They are process
//     global and must not change afterwards.
// ---------------------------------------------------------------------------

// Identifies one partition subgraph: (partitioned parent table ID, partition index).
struct PartitionRef {
    table_id_t parentTableID = INVALID_TABLE_ID;
    uint64_t partitionIndex = 0;
};

using PartitionHandle = void*;

// Everything the engine needs to read one remotely-routed partition.
// `scanFunction` must point to storage owned by the wrapper that stays valid for the
// process lifetime (a static object is fine).
// `createBindData(nodeUniqueName)` must return bind data whose columns expose one
// INTERNAL-ID column named "<nodeUniqueName>._ID" plus one column per parent
// property named "<nodeUniqueName>.<propertyName>", in parent schema order - the same
// convention extension-provided foreign tables follow - so planner schema lookups on
// node property expressions resolve against the scan output.
struct PartitionScanSpec {
    const function::TableFunction* scanFunction = nullptr;
    std::function<std::unique_ptr<function::TableFuncBindData>(const std::string& nodeUniqueName)>
        createBindData = nullptr;
};

struct PartitionRoutingHooks {
    // Wrapper-owned state, passed back to every callback.
    void* context = nullptr;

    // --- Placement ------------------------------------------------------------
    // Called before the engine touches partition `ref` for any purpose (storage
    // creation, scan binding, write routing, lifecycle notification).
    // Return true + set *handleOut -> the wrapper owns this partition (remote).
    // Return false -> local storage, exactly as today.
    // Must be side-effect free and consistent: the same ref must always yield
    // the same answer, and the same handle for claimed refs.
    bool (*locate)(void* context, PartitionRef ref, PartitionHandle* handleOut) = nullptr;

    // --- Lifecycle ------------------------------------------------------------
    // Notification that a partition subgraph entry was created / dropped in the
    // catalog. Create fires for EVERY partition (this is how a wrapper learns
    // about newly provisioned partitions and decides where they live); drop
    // fires for partitions the wrapper claims via locate(). Fire-and-forget;
    // the engine proceeds regardless of the outcome. Renames are not reported
    // because PartitionRef is ID-based and IDs survive renames.
    void (*onPartitionCreate)(void* context, PartitionRef ref, PartitionHandle handle) = nullptr;
    void (*onPartitionDrop)(void* context, PartitionRef ref, PartitionHandle handle) = nullptr;

    // --- Reads ----------------------------------------------------------------
    // Called at bind time for each claimed partition of a scanned parent. The
    // wrapper fills *specOut with its own scan function plus a bind-data factory.
    // The engine attaches them to an internal clone of the partition's catalog
    // entry (keeping schema, table ID, and partition lineage). Mixed local/remote
    // scans of one parent are rejected at bind time.
    bool (*bindScan)(void* context, PartitionRef ref, PartitionHandle handle,
        PartitionScanSpec* specOut) = nullptr;

    // --- Point writes (INSERT / MERGE-create) ----------------------------------
    // Called instead of a local NodeTable::insert for a claimed partition. The
    // key and column vectors are already evaluated and hold exactly one row.
    // The wrapper ships the row and returns the remotely-assigned node ID
    // (offset + child table ID), which flows into the output vector as usual.
    nodeID_t (*insertRow)(void* context, PartitionRef ref, PartitionHandle handle,
        transaction::Transaction* tx, const ValueVector* keyVector,
        std::span<ValueVector* const> columnVectors) = nullptr;

    // --- Bulk writes (COPY FROM / INSERT ... SELECT) ---------------------------
    // Same contract as insertRow but for a run of numRows consecutive logical rows starting at
    // startRow. Row j of the run lives at selection position
    // `vec->state->getSelVector()[startRow + j]` of every column vector - the same convention
    // InMemChunkedNodeGroup::append uses. PK/uniqueness validation is the wrapper's
    // responsibility for claimed partitions.
    void (*insertChunk)(void* context, PartitionRef ref, PartitionHandle handle,
        transaction::Transaction* tx, const ValueVector* keyVector,
        std::span<ValueVector* const> columnVectors, uint64_t startRow, uint64_t numRows) = nullptr;

    // --- Lookups (MERGE-match output materialization) ---------------------------
    // Fetch an existing remote row by node ID into the output vectors. Return
    // false if the row does not exist. If unset, lookups against claimed
    // partitions fail with "not supported".
    bool (*lookupRow)(void* context, PartitionRef ref, PartitionHandle handle,
        transaction::Transaction* tx, nodeID_t nodeID,
        std::span<ValueVector*> outputVectors) = nullptr;
};

// Process-global registration. Call before opening any Database; installing
// hooks twice without resetting to nullptr first is an error. The registry keeps
// the raw pointer, so the hooks object must outlive the registration (typically
// static or owned by the wrapper for the process lifetime).
LBUG_API void setPartitionRoutingHooks(const PartitionRoutingHooks* hooks);
LBUG_API const PartitionRoutingHooks* getPartitionRoutingHooks();

} // namespace common
} // namespace lbug

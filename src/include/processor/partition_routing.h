#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/vector/value_vector.h"

namespace lbug {
namespace common {
class ValueVector;
}
namespace catalog {
class NodeTableCatalogEntry;
}
namespace main {
class ClientContext;
}
namespace storage {
class NodeTable;
}
namespace transaction {
class Transaction;
}

namespace processor {

// Computes a partition index in [0, numPartitions) for every selected row of `keyVector`.
//
// HASH partitions use the same value hashing as the built-in HASH() function, so a given
// partition-key value always lands in the same partition. RANGE is refused at DDL time (see
// docs/partitioning.md). LIST partitions do not use this function: their partition set grows
// dynamically, so they route through ListPartitionRouter below.
void computePartitionIndexes(const common::ValueVector& keyVector, uint64_t numPartitions,
    std::vector<uint64_t>& outIndexes);

// Encodes the selected value of `keyVector` into a stable opaque byte string used as the
// LIST-partition lookup key. The encoding must be injective per physical type: fixed-width values
// encode as their little-endian representation, strings/blobs as length-prefixed bytes.
std::string encodeListPartitionKey(const common::ValueVector& keyVector, uint32_t pos);

// Creates list partitions on first sight of a new partition-key value.
//
// A LIST parent starts with zero partitions; every distinct encoded key gets its own node-table
// subgraph, created inside the caller's transaction with the same machinery as CREATE NODE TABLE
// (catalog entry + serial sequence + subgraph + storage + WAL create record), so rollback,
// replay, and checkpointing behave exactly like ordinary DDL.
//
// The router is not thread-safe by itself; COPY workers share one instance and its internal mutex
// serializes creation. Concurrent transactions racing to create the same value surface a
// write-write conflict at commit time (both modify the parent's catalog entry).
class ListPartitionRouter {
public:
    ListPartitionRouter(main::ClientContext* context, catalog::NodeTableCatalogEntry* parent);

    struct Route {
        storage::NodeTable* table;
        // Ordinal of the partition == index into the parent's childTableIDs order. Stable:
        // partitions are append-only.
        uint64_t ordinal;
    };

    // Encode + resolve-or-create in one step.
    Route route(const common::ValueVector& keyVector, uint32_t pos);
    // Resolve-or-create for an already-encoded key. Locks the router.
    Route getOrCreatePartition(const std::string& encodedKey);
    // Same, for a caller already holding the lock (see withLock).
    Route getOrCreatePartitionLocked(const std::string& encodedKey);

    // Runs `fn` under the router's mutex. COPY workers use this to atomically resolve routes AND
    // grow their target arrays (shared + per-worker), so two workers discovering new partitions
    // cannot interleave array growth. `fn` must not route again through getOrCreatePartition.
    template<typename F>
    auto withLock(F&& fn) -> decltype(fn()) {
        std::lock_guard lck{mtx};
        return fn();
    }

    std::mutex mtx;
    main::ClientContext* context;
    catalog::NodeTableCatalogEntry* parent;
    std::unordered_map<std::string, Route> routesByKey;
};

} // namespace processor
} // namespace lbug

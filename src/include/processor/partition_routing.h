#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/partition_routing.h"
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
// docs/partitioning.md). LIST and daily partitions do not use this function: their partition sets
// grow dynamically, so they route through DynamicPartitionRouter below.
void computePartitionIndexes(const common::ValueVector& keyVector, uint64_t numPartitions,
    std::vector<uint64_t>& outIndexes);

// Encodes the selected value of `keyVector` into a stable opaque byte string used as the
// LIST-partition lookup key. The encoding must be injective per physical type: fixed-width values
// encode as their little-endian representation, strings/blobs as length-prefixed bytes.
std::string encodeListPartitionKey(const common::ValueVector& keyVector, uint32_t pos);

// Day zero is 1970-01-01. rawTicks must be in the column's native unit.
int64_t getPartitionDay(int64_t rawTicks, common::LogicalTypeID type);
std::string encodeDayPartitionKey(int64_t day);
int64_t decodeDayPartitionKey(std::string_view encodedKey);

inline bool usesDynamicPartitionRouting(common::PartitionMethod method) {
    return method == common::PartitionMethod::LIST || method == common::PartitionMethod::RANGE_DAY;
}

// Direct writes to a child must satisfy the same routing rule as writes through its parent.
// Validate the entire selected vector before appending any of its rows.
void validatePartitionKey(main::ClientContext* context, const catalog::NodeTableCatalogEntry& child,
    const common::ValueVector& keyVector);

// Creates LIST or daily partitions on first sight of a new key or day.
//
// Every distinct encoded key gets its own node-table
// subgraph, created inside the caller's transaction with the same machinery as CREATE NODE TABLE
// (catalog entry + serial sequence + subgraph + storage + WAL create record), so rollback,
// replay, and checkpointing behave exactly like ordinary DDL.
//
// The router is not thread-safe by itself; COPY workers share one instance and its internal mutex
// serializes creation. Concurrent transactions racing to create the same value surface a
// write-write conflict when acquiring the writable parent catalog entry.
class DynamicPartitionRouter {
public:
    DynamicPartitionRouter(main::ClientContext* context, catalog::NodeTableCatalogEntry* parent);

    struct Route {
        storage::NodeTable* table;
        // Ordinal of the partition == index into the parent's childTableIDs order. Stable:
        // partitions are append-only.
        uint64_t ordinal;
    };

    // Encode + resolve-or-create in one step.
    Route route(const common::ValueVector& keyVector, uint32_t pos);
    Route routeLocked(const common::ValueVector& keyVector, uint32_t pos);

    // Runs `fn` under the router's mutex. COPY workers use this to atomically resolve routes AND
    // grow their target arrays (shared + per-worker), so two workers discovering new partitions
    // cannot interleave array growth. `fn` must use routeLocked to avoid relocking.
    template<typename F>
    auto withLock(F&& fn) -> decltype(fn()) {
        std::lock_guard lck{mtx};
        return fn();
    }

private:
    Route getOrCreatePartitionLocked(const std::string& encodedKey);

    std::mutex mtx;
    main::ClientContext* context;
    catalog::NodeTableCatalogEntry* parent;
    std::unordered_map<std::string, Route> routesByKey;
};

} // namespace processor
} // namespace lbug

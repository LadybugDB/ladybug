#pragma once

#include <cstdint>
#include <vector>

#include "common/types/types.h"

namespace lbug {
namespace common {

// PostgreSQL-style partitioning method. Mirrors binder::BoundPartitionMethod (HASH=0, RANGE=1,
// LIST=2).
enum class PartitionMethod : uint8_t { HASH = 0, RANGE = 1, LIST = 2 };

// Write-routing metadata for a partitioned node table. The logical parent owns no physical
// storage; every write to the parent must be routed into one of its partition subgraphs.
//
// `partitionKeyColumnID` is the catalog property id (which, for a node table, also indexes the
// write's property columns in evaluation order). `partitionTableIDs` holds the child table ids in
// partition order (index == partition index).
struct NodePartitionWriteInfo {
    PartitionMethod method;
    // Owning partitioned parent. Used to address partitions via PartitionRef.
    common::table_id_t parentTableID;
    common::column_id_t partitionKeyColumnID;
    uint64_t numPartitions;
    std::vector<common::table_id_t> partitionTableIDs;

    NodePartitionWriteInfo() = default;
    NodePartitionWriteInfo(PartitionMethod method, common::table_id_t parentTableID,
        common::column_id_t partitionKeyColumnID, uint64_t numPartitions,
        std::vector<common::table_id_t> partitionTableIDs)
        : method{method}, parentTableID{parentTableID}, partitionKeyColumnID{partitionKeyColumnID},
          numPartitions{numPartitions}, partitionTableIDs{std::move(partitionTableIDs)} {}
};

} // namespace common
} // namespace lbug
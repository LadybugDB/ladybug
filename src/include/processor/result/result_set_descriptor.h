#pragma once

#include <atomic>

#include "common/types/types.h"

namespace lbug {
namespace planner {
class Schema;
} // namespace planner

namespace processor {

struct DataChunkDescriptor {
    bool isSingleState;
    std::vector<common::LogicalType> logicalTypes;

    explicit DataChunkDescriptor(bool isSingleState) : isSingleState{isSingleState} {}
    DataChunkDescriptor(const DataChunkDescriptor& other)
        : isSingleState{other.isSingleState},
          logicalTypes(common::LogicalType::copy(other.logicalTypes)) {}

    inline std::unique_ptr<DataChunkDescriptor> copy() const {
        return std::make_unique<DataChunkDescriptor>(*this);
    }
};

struct LBUG_API ResultSetDescriptor {
    // Monotonically increasing identity that survives pointer reuse (ABA
    // prevention).  Thread-local ResultSet caching in ProcessorTask::run()
    // compares this ID instead of the raw pointer so that a descriptor
    // allocated at the same address as a freed one is never confused with it.
    // copy() preserves the id: a copy semantically represents the same descriptor
    // slot (same plan position, same content). In particular the per-execution
    // re-attach of cached-plan sink descriptors (ClientContext::attachSinkDescriptors)
    // must carry the same id across executions, otherwise the thread-local cache
    // would miss on every execution of the same prepared statement.
    static inline std::atomic<uint64_t> nextID{0};
    uint64_t id;

    std::vector<std::unique_ptr<DataChunkDescriptor>> dataChunkDescriptors;

    ResultSetDescriptor() : id{nextID.fetch_add(1, std::memory_order_relaxed)} {}
    explicit ResultSetDescriptor(
        std::vector<std::unique_ptr<DataChunkDescriptor>> dataChunkDescriptors)
        : id{nextID.fetch_add(1, std::memory_order_relaxed)},
          dataChunkDescriptors{std::move(dataChunkDescriptors)} {}
    explicit ResultSetDescriptor(planner::Schema* schema);
    DELETE_BOTH_COPY(ResultSetDescriptor);

    std::unique_ptr<ResultSetDescriptor> copy() const;

    static std::unique_ptr<ResultSetDescriptor> EmptyDescriptor() {
        return std::make_unique<ResultSetDescriptor>();
    }
};

} // namespace processor
} // namespace lbug

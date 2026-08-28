#include "processor/result/result_set_descriptor.h"

#include "planner/operator/schema.h"

namespace lbug {
namespace processor {

ResultSetDescriptor::ResultSetDescriptor(planner::Schema* schema)
    : id{nextID.fetch_add(1, std::memory_order_relaxed)} {
    for (auto i = 0u; i < schema->getNumGroups(); ++i) {
        auto group = schema->getGroup(i);
        auto dataChunkDescriptor = std::make_unique<DataChunkDescriptor>(group->isSingleState());
        for (auto& expression : group->getExpressions()) {
            dataChunkDescriptor->logicalTypes.push_back(expression->getDataType().copy());
        }
        dataChunkDescriptors.push_back(std::move(dataChunkDescriptor));
    }
}

std::unique_ptr<ResultSetDescriptor> ResultSetDescriptor::copy() const {
    std::vector<std::unique_ptr<DataChunkDescriptor>> dataChunkDescriptorsCopy;
    dataChunkDescriptorsCopy.reserve(dataChunkDescriptors.size());
    for (auto& dataChunkDescriptor : dataChunkDescriptors) {
        dataChunkDescriptorsCopy.push_back(
            std::make_unique<DataChunkDescriptor>(*dataChunkDescriptor));
    }
    auto descriptorCopy =
        std::make_unique<ResultSetDescriptor>(std::move(dataChunkDescriptorsCopy));
    // A copy is the same descriptor slot with identical content (copies are never mutated
    // afterwards), so it inherits the source's identity. This keeps the id stable across
    // executions of a prepared statement's cached plan, letting the thread-local ResultSet
    // cache in ProcessorTask::run() actually hit. Two live descriptors with the same id are
    // by construction content-identical, so cache reuse remains safe.
    descriptorCopy->id = id;
    return descriptorCopy;
}

} // namespace processor
} // namespace lbug

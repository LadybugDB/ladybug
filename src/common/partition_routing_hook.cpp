#include "common/partition_routing_hook.h"

#include "common/exception/exception.h"

namespace lbug {
namespace common {

namespace {

const PartitionRoutingHooks* hooks_ = nullptr;

} // namespace

void setPartitionRoutingHooks(const PartitionRoutingHooks* hooks) {
    if (hooks != nullptr && hooks_ != nullptr) {
        // A wrapper may reset to nullptr (e.g. between tests) but two different
        // hook sets must never be live at once: routing answers are cached by
        // the engine and assumed stable.
        throw Exception("Partition routing hooks are already installed.");
    }
    hooks_ = hooks;
}

const PartitionRoutingHooks* getPartitionRoutingHooks() {
    return hooks_;
}

} // namespace common
} // namespace lbug

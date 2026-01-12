#include "graph/graph_entry_set.h"

#include "common/exception/runtime.h"
#include "main/client_context.h"
#include <format>

using namespace lbug::common;

namespace lbug {
namespace graph {

void GraphEntrySet::validateGraphNotExist(const std::string& name) const {
    if (hasGraph(name)) {
        throw RuntimeException(std::format("Projected graph {} already exists.", name));
    }
}

void GraphEntrySet::validateGraphExist(const std::string& name) const {
    if (!hasGraph(name)) {
        throw RuntimeException(std::format("Projected graph {} does not exists.", name));
    }
}

GraphEntrySet* GraphEntrySet::Get(const main::ClientContext& context) {
    return context.graphEntrySet.get();
}

} // namespace graph
} // namespace lbug

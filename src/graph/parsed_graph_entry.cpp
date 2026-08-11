#include "graph/parsed_graph_entry.h"

#include "main/query_result.h"

using namespace lbug::common;

namespace lbug {
namespace graph {

// Defined here so the shared_ptr<QueryResult> members destroy against a complete type.
ParsedNativeGraphEntry::~ParsedNativeGraphEntry() = default;

std::string GraphEntryTypeUtils::toString(GraphEntryType type) {
    switch (type) {
    case GraphEntryType::NATIVE:
        return "NATIVE";
    case GraphEntryType::CYPHER:
        return "CYPHER";
    default:
        UNREACHABLE_CODE;
    }
}

} // namespace graph
} // namespace lbug

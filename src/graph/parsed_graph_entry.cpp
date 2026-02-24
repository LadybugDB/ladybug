#include "graph/parsed_graph_entry.h"

using namespace lbug::common;

namespace lbug {
namespace graph {

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

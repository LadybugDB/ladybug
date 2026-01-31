#include "catalog/catalog_entry/graph_catalog_entry.h"

#include "common/serializer/deserializer.h"
#include <format>

using namespace lbug::common;

namespace lbug {
namespace catalog {

void GraphCatalogEntry::serialize(Serializer& serializer) const {
    CatalogEntry::serialize(serializer);
    serializer.writeDebuggingInfo("isAnyGraph");
    serializer.write(isAnyGraph);
}

std::unique_ptr<GraphCatalogEntry> GraphCatalogEntry::deserialize(Deserializer& deserializer) {
    std::string debuggingInfo;
    bool isAnyGraph = false;
    deserializer.validateDebuggingInfo(debuggingInfo, "isAnyGraph");
    deserializer.deserializeValue(isAnyGraph);
    auto result = std::make_unique<GraphCatalogEntry>();
    result->isAnyGraph = isAnyGraph;
    return result;
}

std::string GraphCatalogEntry::toCypher(const ToCypherInfo& /* info */) const {
    return std::format("DROP GRAPH IF EXISTS `{}`;\n"
                       "CREATE GRAPH `{}` {};\n",
        getName(), getName(), isAnyGraph ? "ANY" : "");
}

} // namespace catalog
} // namespace lbug

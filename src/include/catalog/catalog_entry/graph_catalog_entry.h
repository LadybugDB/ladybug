#pragma once

#include "catalog_entry.h"

namespace lbug {
namespace catalog {

class LBUG_API GraphCatalogEntry final : public CatalogEntry {
public:
    //===--------------------------------------------------------------------===//
    // constructors
    //===--------------------------------------------------------------------===//
    GraphCatalogEntry() : CatalogEntry{CatalogEntryType::GRAPH_ENTRY, ""}, isAnyGraph{false} {}
    GraphCatalogEntry(std::string graphName, bool isAnyGraph)
        : CatalogEntry{CatalogEntryType::GRAPH_ENTRY, std::move(graphName)},
          isAnyGraph{isAnyGraph} {}

    //===--------------------------------------------------------------------===//
    // getter & setter
    //===--------------------------------------------------------------------===//
    bool isAnyGraphType() const { return isAnyGraph; }

    //===--------------------------------------------------------------------===//
    // serialization & deserialization
    //===--------------------------------------------------------------------===//
    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<GraphCatalogEntry> deserialize(common::Deserializer& deserializer);

    std::string toCypher(const ToCypherInfo& info) const override;

private:
    bool isAnyGraph;
};

} // namespace catalog
} // namespace lbug

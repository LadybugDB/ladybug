#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/cast.h"

namespace lbug {
namespace main {
class QueryResult;
} // namespace main
namespace graph {

enum class GraphEntryType : uint8_t {
    NATIVE = 0,
    CYPHER = 1,
};

struct GraphEntryTypeUtils {
    static std::string toString(GraphEntryType type);
};

struct LBUG_API ParsedGraphEntry {
    GraphEntryType type;

    explicit ParsedGraphEntry(GraphEntryType type) : type{type} {}
    virtual ~ParsedGraphEntry() = default;

    template<class TARGET>
    TARGET& cast() {
        return common::dynamic_cast_checked<TARGET&>(*this);
    }
};

struct ParsedNativeGraphTableInfo {
    std::string tableName;
    std::string predicate;

    ParsedNativeGraphTableInfo(std::string tableName, std::string predicate)
        : tableName{std::move(tableName)}, predicate{std::move(predicate)} {}
};

struct LBUG_API ParsedNativeGraphEntry : ParsedGraphEntry {
    std::vector<ParsedNativeGraphTableInfo> nodeInfos;
    std::vector<ParsedNativeGraphTableInfo> relInfos;
    // Arrow CSR per relInfos[i] (same order), materialized at PROJECT_GRAPH time by running the
    // projection scan through the arrow CSR collector; entries are ArrowQueryResults whose
    // CSRMetadata GDS consumers wrap zero-copy. Null (or empty) when materialization was skipped
    // (multi-node-table graph, per-table predicate, manual transaction) — consumers must fall
    // back to scanning storage. Lifetime: the session's GraphEntrySet; freed on DROP.
    std::vector<std::shared_ptr<main::QueryResult>> relCsrResults;
    // Each rel table's storage changeEpoch, captured immediately BEFORE the materializing scan
    // (parallel to relCsrResults; meaningless where the result is null). Consumers compare it to
    // the table's current epoch and treat any mismatch as staleness, falling back to scanning
    // live storage. Capturing before the scan makes a concurrent mutation during materialization
    // read as stale — conservative in the safe direction.
    std::vector<uint64_t> relCsrEpochs;

    ParsedNativeGraphEntry(std::vector<ParsedNativeGraphTableInfo> nodeInfos,
        std::vector<ParsedNativeGraphTableInfo> relInfos)
        : ParsedGraphEntry{GraphEntryType::NATIVE}, nodeInfos{std::move(nodeInfos)},
          relInfos{std::move(relInfos)} {}
    ~ParsedNativeGraphEntry() override;
};

struct LBUG_API ParsedCypherGraphEntry : ParsedGraphEntry {
    std::string cypherQuery;

    explicit ParsedCypherGraphEntry(std::string cypherQuery)
        : ParsedGraphEntry{GraphEntryType::CYPHER}, cypherQuery{std::move(cypherQuery)} {}
};

} // namespace graph
} // namespace lbug

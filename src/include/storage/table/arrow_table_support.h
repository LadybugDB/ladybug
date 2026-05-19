#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/api.h"
#include "common/arrow/arrow.h"
#include "main/connection.h"
#include "storage/table/arrow_csr_rel_data.h"

namespace lbug {

// Result of creating an arrow table view
struct ArrowTableCreationResult {
    std::unique_ptr<main::QueryResult> queryResult;
    std::string arrowId;
};

class LBUG_API ArrowTableSupport {
public:
    // ── Node / EdgeList rel registry ─────────────────────────────────────
    static std::string registerArrowData(ArrowSchemaWrapper schema,
        std::vector<ArrowArrayWrapper> arrays);
    static bool getArrowData(const std::string& id, ArrowSchemaWrapper*& schema,
        std::vector<ArrowArrayWrapper>*& arrays);
    static void unregisterArrowData(const std::string& id);

    // ── CSR rel registry ─────────────────────────────────────────────────
    static std::string registerCsrRelData(storage::ArrowCsrRelData data);
    static const storage::ArrowCsrRelData* getCsrRelData(const std::string& id);
    static void unregisterCsrRelData(const std::string& id);

    // ── Table creation helpers ────────────────────────────────────────────

    // Create a node table view from Arrow C Data Interface structures.
    static ArrowTableCreationResult createViewFromArrowTable(main::Connection& connection,
        const std::string& viewName, ArrowSchemaWrapper schema,
        std::vector<ArrowArrayWrapper> arrays);

    // Create an edge-list rel table from Arrow C Data Interface structures.
    // The Arrow table must contain "from" and "to" endpoint columns (PK values).
    static ArrowTableCreationResult createRelTableFromArrowTable(main::Connection& connection,
        const std::string& tableName, const std::string& srcTableName,
        const std::string& dstTableName, ArrowSchemaWrapper schema,
        std::vector<ArrowArrayWrapper> arrays, const std::string& srcColumnName = "from",
        const std::string& dstColumnName = "to");

    // Create a CSR-layout rel table from Arrow C Data Interface structures.
    // fwdIndices/fwdIndptr are required; bwd* are optional for O(degree) BWD scans.
    // fwdIndices: struct with child[0]=UINT64 dst_node_offset, child[1..]=edge properties
    // fwdIndptr:  struct with child[0]=UINT64 row pointers (N+1 entries)
    // bwd*:       same layout but dst-grouped (child[0]=src_node_offset)
    static ArrowTableCreationResult createArrowCsrRelTable(main::Connection& connection,
        const std::string& tableName, const std::string& srcTableName,
        const std::string& dstTableName, ArrowSchemaWrapper fwdIndicesSchema,
        std::vector<ArrowArrayWrapper> fwdIndices, ArrowSchemaWrapper fwdIndptrSchema,
        std::vector<ArrowArrayWrapper> fwdIndptr,
        std::optional<ArrowSchemaWrapper> bwdIndicesSchema = std::nullopt,
        std::optional<std::vector<ArrowArrayWrapper>> bwdIndices = std::nullopt,
        std::optional<ArrowSchemaWrapper> bwdIndptrSchema = std::nullopt,
        std::optional<std::vector<ArrowArrayWrapper>> bwdIndptr = std::nullopt);

    // Drop a table and clean up its Arrow registry entry.
    static std::unique_ptr<main::QueryResult> unregisterArrowTable(main::Connection& connection,
        const std::string& tableName);
};

} // namespace lbug

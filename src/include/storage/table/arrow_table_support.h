#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/api.h"
#include "common/arrow/arrow.h"
#include "main/connection.h"

namespace lbug {

enum class ArrowRelTableLayout : uint8_t { FLAT, CSR };

struct ArrowRelTableData {
    ArrowRelTableLayout layout = ArrowRelTableLayout::FLAT;
    ArrowSchemaWrapper schema;
    std::vector<ArrowArrayWrapper> arrays;
    ArrowSchemaWrapper indptrSchema;
    std::vector<ArrowArrayWrapper> indptrArrays;
    std::string dstColumnName = "to";
};

// Registry-owned Arrow node-table payload. Stored as a shared_ptr in the
// process-wide registry so that tables can pin the data they view: the
// shared_ptr copy returned by getArrowData()/getArrowRelData() keeps the
// underlying Arrow buffers alive even if another thread unregisters (erases)
// the registry entry concurrently (CWE-416/667, issue #933).
struct ArrowTableData {
    ArrowSchemaWrapper schema;
    std::vector<ArrowArrayWrapper> arrays;
};

// Result of creating an arrow table view
struct ArrowTableCreationResult {
    std::unique_ptr<main::QueryResult> queryResult;
    std::string arrowId;
};

class LBUG_API ArrowTableSupport {
public:
    // Register Arrow data and return an ID
    static std::string registerArrowData(ArrowSchemaWrapper schema,
        std::vector<ArrowArrayWrapper> arrays);

    // Register Arrow relationship data and return an ID
    static std::string registerArrowRelData(ArrowRelTableData data);

    // Retrieve Arrow data by ID. Returns a shared_ptr pinning the registry
    // entry's lifetime: the caller may safely dereference it after the
    // registry lock is released, even if another thread concurrently
    // unregisters the same ID. Returns nullptr when the ID is unknown.
    static std::shared_ptr<ArrowTableData> getArrowData(const std::string& id);

    // Retrieve Arrow relationship data by ID (same lifetime semantics as
    // getArrowData above). Returns nullptr when the ID is unknown.
    static std::shared_ptr<ArrowRelTableData> getArrowRelData(const std::string& id);

    // Unregister Arrow data by ID
    static void unregisterArrowData(const std::string& id);

    // Create a view from Arrow C Data Interface structures
    static ArrowTableCreationResult createViewFromArrowTable(main::Connection& connection,
        const std::string& viewName, ArrowSchemaWrapper schema,
        std::vector<ArrowArrayWrapper> arrays);

    // Create a relationship table from Arrow C Data Interface structures.
    // The Arrow table must contain source/destination endpoint columns.
    static ArrowTableCreationResult createRelTableFromArrowTable(main::Connection& connection,
        const std::string& tableName, const std::string& srcTableName,
        const std::string& dstTableName, ArrowSchemaWrapper schema,
        std::vector<ArrowArrayWrapper> arrays, const std::string& srcColumnName = "from",
        const std::string& dstColumnName = "to");

    // Create a relationship table from Arrow CSR arrays. The indices table must contain a
    // destination offset column and any relationship property columns. The indptr table must
    // contain one offset column with source-node row offsets into the indices table.
    static ArrowTableCreationResult createRelTableFromArrowCSR(main::Connection& connection,
        const std::string& tableName, const std::string& srcTableName,
        const std::string& dstTableName, ArrowSchemaWrapper indicesSchema,
        std::vector<ArrowArrayWrapper> indicesArrays, ArrowSchemaWrapper indptrSchema,
        std::vector<ArrowArrayWrapper> indptrArrays, const std::string& dstColumnName = "to");

    // Unregister an arrow table completely (drop table and unregister data)
    static std::unique_ptr<main::QueryResult> unregisterArrowTable(main::Connection& connection,
        const std::string& tableName);
};

} // namespace lbug

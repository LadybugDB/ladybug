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

    // Retrieve Arrow data by ID (returns pointers to data in registry)
    static bool getArrowData(const std::string& id, ArrowSchemaWrapper*& schema,
        std::vector<ArrowArrayWrapper>*& arrays);

    // Retrieve Arrow relationship data by ID (returns pointer to data in registry)
    static bool getArrowRelData(const std::string& id, ArrowRelTableData*& data);

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

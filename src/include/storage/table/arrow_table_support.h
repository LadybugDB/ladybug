#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/arrow/arrow.h"
#include "main/connection.h"

namespace lbug {

// Result of creating an arrow table view
struct ArrowTableCreationResult {
    std::unique_ptr<main::QueryResult> queryResult;
    std::string arrowId;
};

class ArrowTableSupport {
public:
    // Register Arrow data and return an ID
    static std::string registerArrowData(ArrowSchema& schema, std::vector<ArrowArray>& arrays);

    // Retrieve Arrow data by ID (returns pointers to data in registry)
    static bool getArrowData(const std::string& id, ArrowSchemaWrapper*& schema,
        std::vector<ArrowArrayWrapper>*& arrays);

    // Unregister Arrow data by ID
    static void unregisterArrowData(const std::string& id);

    // Create a view from Arrow C Data Interface structures
    static ArrowTableCreationResult createViewFromArrowTable(main::Connection& connection,
        const std::string& viewName, ArrowSchema& schema, std::vector<ArrowArray>& arrays);

    // Unregister an arrow table completely (drop table and unregister data)
    static std::unique_ptr<main::QueryResult> unregisterArrowTable(main::Connection& connection,
        const std::string& tableName);
};

} // namespace lbug
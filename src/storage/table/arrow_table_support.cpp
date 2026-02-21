#include "storage/table/arrow_table_support.h"

#include <mutex>
#include <unordered_map>

#include "common/arrow/arrow_converter.h"
#include "common/exception/runtime.h"
#include "main/database.h"

namespace lbug {

// Global registry for Arrow table data
// Memory Management:
// - Registry owns the Arrow data (ArrowSchemaWrapper/ArrowArrayWrapper with release callbacks)
// - ArrowNodeTable stores shallow copies (no release callbacks) and the arrowId
// - When a table is dropped (via DROP TABLE or unregisterArrowTable), ArrowNodeTable's
//   destructor automatically calls unregisterArrowData to clean up the registry entry
// - The wrappers' destructors call the release callbacks to free the actual Arrow memory
static std::mutex g_arrowRegistryMutex;
static std::unordered_map<std::string,
    std::pair<ArrowSchemaWrapper, std::vector<ArrowArrayWrapper>>>
    g_arrowRegistry;

std::string join(const std::vector<std::string>& strings, const std::string& delimiter) {
    if (strings.empty())
        return "";
    std::string result = strings[0];
    for (size_t i = 1; i < strings.size(); i++) {
        result += delimiter + strings[i];
    }
    return result;
}

static int64_t findArrowColumnByName(const ArrowSchemaWrapper& schema, const std::string& name) {
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (schema.children && schema.children[i] && schema.children[i]->name &&
            name == schema.children[i]->name) {
            return i;
        }
    }
    return -1;
}

std::string ArrowTableSupport::registerArrowData(ArrowSchemaWrapper schema,
    std::vector<ArrowArrayWrapper> arrays) {
    std::lock_guard<std::mutex> lock(g_arrowRegistryMutex);

    // Generate a unique ID
    static size_t nextId = 0;
    std::string id = "arrow_" + std::to_string(nextId++);

    // Store in registry
    g_arrowRegistry[id] = std::make_pair(std::move(schema), std::move(arrays));

    return id;
}

bool ArrowTableSupport::getArrowData(const std::string& id, ArrowSchemaWrapper*& schema,
    std::vector<ArrowArrayWrapper>*& arrays) {
    std::lock_guard<std::mutex> lock(g_arrowRegistryMutex);

    auto it = g_arrowRegistry.find(id);
    if (it == g_arrowRegistry.end()) {
        return false;
    }

    // Return pointers to the data in the registry (not copies)
    schema = &it->second.first;
    arrays = &it->second.second;
    return true;
}

void ArrowTableSupport::unregisterArrowData(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_arrowRegistryMutex);
    g_arrowRegistry.erase(id);
}

ArrowTableCreationResult ArrowTableSupport::createViewFromArrowTable(main::Connection& connection,
    const std::string& viewName, ArrowSchemaWrapper schema, std::vector<ArrowArrayWrapper> arrays) {

    // Get table info from Arrow C Data Interface
    int64_t numColumns = schema.n_children;

    // Build column definitions for CREATE NODE TABLE statement
    std::vector<std::string> columnDefs;
    for (int64_t i = 0; i < numColumns; i++) {
        std::string colName = schema.children[i]->name;
        std::string colType =
            common::ArrowConverter::fromArrowSchema(schema.children[i]).toString();
        columnDefs.push_back(colName + " " + colType);
    }

    // Add PRIMARY KEY clause using first column
    std::string primaryKey = numColumns > 0 ? schema.children[0]->name : "id";
    columnDefs.push_back("PRIMARY KEY (" + primaryKey + ")");

    // Create table definition
    std::string tableDef = "(" + join(columnDefs, ", ") + ")";

    // Register the Arrow data and get an ID
    std::string arrowId = registerArrowData(std::move(schema), std::move(arrays));

    // Build CREATE NODE TABLE statement with arrow storage

    std::string statement = "CREATE NODE TABLE " + viewName + " " + tableDef +
                            " WITH (storage='arrow://" + arrowId + "')";

    // Create table with Arrow storage
    auto queryResult = connection.query(statement);
    if (!queryResult->isSuccess()) {
        unregisterArrowData(arrowId);
    }

    return {std::move(queryResult), arrowId};
}

ArrowTableCreationResult ArrowTableSupport::createRelTableFromArrowTable(
    main::Connection& connection, const std::string& tableName, const std::string& srcTableName,
    const std::string& dstTableName, ArrowSchemaWrapper schema,
    std::vector<ArrowArrayWrapper> arrays, const std::string& srcColumnName,
    const std::string& dstColumnName) {
    if (srcColumnName != "from" || dstColumnName != "to") {
        throw common::RuntimeException(
            "Arrow relationship registration currently requires endpoint columns named 'from' and "
            "'to'");
    }

    int64_t numColumns = schema.n_children;
    if (numColumns < 2) {
        throw common::RuntimeException(
            "Arrow relationship table must contain at least source and destination columns");
    }

    auto srcColIdx = findArrowColumnByName(schema, srcColumnName);
    auto dstColIdx = findArrowColumnByName(schema, dstColumnName);
    if (srcColIdx < 0 || dstColIdx < 0) {
        throw common::RuntimeException("Arrow relationship table must include endpoint columns '" +
                                       srcColumnName + "' and '" + dstColumnName + "'");
    }
    if (srcColIdx == dstColIdx) {
        throw common::RuntimeException("Source and destination endpoint columns must be distinct");
    }

    std::vector<std::string> propertyDefs;
    for (int64_t i = 0; i < numColumns; ++i) {
        if (i == srcColIdx || i == dstColIdx) {
            continue;
        }
        std::string colName = schema.children[i]->name;
        std::string colType =
            common::ArrowConverter::fromArrowSchema(schema.children[i]).toString();
        propertyDefs.push_back(colName + " " + colType);
    }

    std::vector<std::string> relDefs;
    relDefs.push_back("FROM " + srcTableName + " TO " + dstTableName);
    relDefs.insert(relDefs.end(), propertyDefs.begin(), propertyDefs.end());
    std::string tableDef = "(" + join(relDefs, ", ") + ")";

    // Register the Arrow data and get an ID.
    std::string arrowId = registerArrowData(std::move(schema), std::move(arrays));

    std::string statement = "CREATE REL TABLE " + tableName + " " + tableDef +
                            " WITH (storage='arrow://" + arrowId + "')";
    auto queryResult = connection.query(statement);
    if (!queryResult->isSuccess()) {
        unregisterArrowData(arrowId);
    }

    return {std::move(queryResult), arrowId};
}

std::unique_ptr<main::QueryResult> ArrowTableSupport::unregisterArrowTable(
    main::Connection& connection, const std::string& tableName) {

    // Drop the table - this will trigger ArrowNodeTable destructor which unregisters the data
    std::string dropStatement = "DROP TABLE " + tableName;
    return connection.query(dropStatement);
}

} // namespace lbug

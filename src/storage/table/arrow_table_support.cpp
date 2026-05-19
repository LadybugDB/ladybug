#include "storage/table/arrow_table_support.h"

#include <mutex>
#include <unordered_map>

#include "common/arrow/arrow_converter.h"
#include "common/exception/runtime.h"
#include "main/database.h"

namespace lbug {

// ── Node / TRIPLES rel registry ──────────────────────────────────────────────
// Memory Management:
// - Registry owns the Arrow data (ArrowSchemaWrapper/ArrowArrayWrapper with release callbacks)
// - Tables store shallow copies (no release callbacks) and keep the arrowId
// - Table destructors call unregisterArrowData / unregisterCsrRelData to free registry entries
static std::mutex g_arrowRegistryMutex;
static std::unordered_map<std::string,
    std::pair<ArrowSchemaWrapper, std::vector<ArrowArrayWrapper>>>
    g_arrowRegistry;

// ── CSR rel registry ──────────────────────────────────────────────────────────
static std::mutex g_csrRegistryMutex;
static std::unordered_map<std::string, storage::ArrowCsrRelData> g_csrRegistry;

static std::string join(const std::vector<std::string>& strings, const std::string& delimiter) {
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

static std::string nextId(const std::string& prefix) {
    static size_t counter = 0;
    return prefix + std::to_string(counter++);
}

// ── Node / TRIPLES rel registry ───────────────────────────────────────────────

std::string ArrowTableSupport::registerArrowData(ArrowSchemaWrapper schema,
    std::vector<ArrowArrayWrapper> arrays) {
    std::lock_guard<std::mutex> lock(g_arrowRegistryMutex);
    std::string id = nextId("arrow_");
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
    schema = &it->second.first;
    arrays = &it->second.second;
    return true;
}

void ArrowTableSupport::unregisterArrowData(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_arrowRegistryMutex);
    g_arrowRegistry.erase(id);
}

// ── CSR rel registry ──────────────────────────────────────────────────────────

std::string ArrowTableSupport::registerCsrRelData(storage::ArrowCsrRelData data) {
    std::lock_guard<std::mutex> lock(g_csrRegistryMutex);
    std::string id = nextId("arrow_csr_");
    g_csrRegistry.emplace(id, std::move(data));
    return id;
}

const storage::ArrowCsrRelData* ArrowTableSupport::getCsrRelData(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_csrRegistryMutex);
    auto it = g_csrRegistry.find(id);
    if (it == g_csrRegistry.end()) {
        return nullptr;
    }
    return &it->second;
}

void ArrowTableSupport::unregisterCsrRelData(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_csrRegistryMutex);
    g_csrRegistry.erase(id);
}

// ── Table creation ────────────────────────────────────────────────────────────

ArrowTableCreationResult ArrowTableSupport::createViewFromArrowTable(main::Connection& connection,
    const std::string& viewName, ArrowSchemaWrapper schema, std::vector<ArrowArrayWrapper> arrays) {

    int64_t numColumns = schema.n_children;
    std::vector<std::string> columnDefs;
    for (int64_t i = 0; i < numColumns; i++) {
        std::string colName = schema.children[i]->name;
        std::string colType =
            common::ArrowConverter::fromArrowSchema(schema.children[i]).toString();
        columnDefs.push_back(colName + " " + colType);
    }

    std::string primaryKey = numColumns > 0 ? schema.children[0]->name : "id";
    columnDefs.push_back("PRIMARY KEY (" + primaryKey + ")");
    std::string tableDef = "(" + join(columnDefs, ", ") + ")";

    std::string arrowId = registerArrowData(std::move(schema), std::move(arrays));
    std::string statement = "CREATE NODE TABLE " + viewName + " " + tableDef +
                            " WITH (storage='arrow://" + arrowId + "')";

    auto queryResult = connection.query(statement);
    if (!queryResult->isSuccess()) {
        unregisterArrowData(arrowId);
    }
    return {std::move(queryResult), std::move(arrowId)};
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

    std::string arrowId = registerArrowData(std::move(schema), std::move(arrays));
    std::string statement = "CREATE REL TABLE " + tableName + " " + tableDef +
                            " WITH (storage='arrow://" + arrowId + "')";
    auto queryResult = connection.query(statement);
    if (!queryResult->isSuccess()) {
        unregisterArrowData(arrowId);
    }
    return {std::move(queryResult), std::move(arrowId)};
}

static void validateCsrAdjacencySchema(const ArrowSchemaWrapper& indicesSchema,
    const ArrowSchemaWrapper& indptrSchema, const std::string& dir) {
    if (indicesSchema.n_children < 1 || !indicesSchema.children || !indicesSchema.children[0] ||
        !indicesSchema.children[0]->format) {
        throw common::RuntimeException(dir + " indices schema must be a struct with at least one "
                                             "UINT64 child (neighbour offset column)");
    }
    if (std::string(indicesSchema.children[0]->format) != "L") {
        throw common::RuntimeException(
            dir + " indices child[0] must be UINT64 (Arrow format 'L') for neighbour offsets");
    }
    if (indptrSchema.n_children < 1 || !indptrSchema.children || !indptrSchema.children[0] ||
        !indptrSchema.children[0]->format) {
        throw common::RuntimeException(
            dir + " indptr schema must be a struct with one UINT64 child");
    }
    if (std::string(indptrSchema.children[0]->format) != "L") {
        throw common::RuntimeException(dir + " indptr child[0] must be UINT64 (Arrow format 'L')");
    }
}

static storage::ArrowCsrAdj buildAdjacency(ArrowSchemaWrapper indicesSchema,
    std::vector<ArrowArrayWrapper> indices, ArrowSchemaWrapper indptrSchema,
    std::vector<ArrowArrayWrapper> indptr) {
    return {std::move(indicesSchema), std::move(indices), std::move(indptrSchema),
        std::move(indptr)};
}

ArrowTableCreationResult ArrowTableSupport::createArrowCsrRelTable(main::Connection& connection,
    const std::string& tableName, const std::string& srcTableName, const std::string& dstTableName,
    ArrowSchemaWrapper fwdIndicesSchema, std::vector<ArrowArrayWrapper> fwdIndices,
    ArrowSchemaWrapper fwdIndptrSchema, std::vector<ArrowArrayWrapper> fwdIndptr,
    std::optional<ArrowSchemaWrapper> bwdIndicesSchema,
    std::optional<std::vector<ArrowArrayWrapper>> bwdIndices,
    std::optional<ArrowSchemaWrapper> bwdIndptrSchema,
    std::optional<std::vector<ArrowArrayWrapper>> bwdIndptr) {

    validateCsrAdjacencySchema(fwdIndicesSchema, fwdIndptrSchema, "FWD");
    if (bwdIndicesSchema.has_value()) {
        if (!bwdIndptrSchema.has_value() || !bwdIndices.has_value() || !bwdIndptr.has_value()) {
            throw common::RuntimeException(
                "BWD CSR data requires all four of: bwdIndicesSchema, bwdIndices, bwdIndptrSchema, "
                "bwdIndptr");
        }
        validateCsrAdjacencySchema(*bwdIndicesSchema, *bwdIndptrSchema, "BWD");
    }

    // Extract property definitions from fwd indices children[1..] (child[0] is dst offset)
    std::vector<std::string> relDefs;
    relDefs.push_back("FROM " + srcTableName + " TO " + dstTableName);
    for (int64_t i = 1; i < fwdIndicesSchema.n_children; ++i) {
        if (!fwdIndicesSchema.children[i] || !fwdIndicesSchema.children[i]->name ||
            !fwdIndicesSchema.children[i]->format) {
            continue;
        }
        std::string colName = fwdIndicesSchema.children[i]->name;
        std::string colType =
            common::ArrowConverter::fromArrowSchema(fwdIndicesSchema.children[i]).toString();
        relDefs.push_back(colName + " " + colType);
    }
    std::string tableDef = "(" + join(relDefs, ", ") + ")";

    storage::ArrowCsrRelData csrData;
    csrData.fwd = buildAdjacency(std::move(fwdIndicesSchema), std::move(fwdIndices),
        std::move(fwdIndptrSchema), std::move(fwdIndptr));
    if (bwdIndicesSchema.has_value()) {
        csrData.bwd = buildAdjacency(std::move(*bwdIndicesSchema), std::move(*bwdIndices),
            std::move(*bwdIndptrSchema), std::move(*bwdIndptr));
    }

    std::string arrowId = registerCsrRelData(std::move(csrData));
    std::string statement = "CREATE REL TABLE " + tableName + " " + tableDef +
                            " WITH (storage='arrow-csr://" + arrowId + "')";
    auto queryResult = connection.query(statement);
    if (!queryResult->isSuccess()) {
        unregisterCsrRelData(arrowId);
    }
    return {std::move(queryResult), std::move(arrowId)};
}

std::unique_ptr<main::QueryResult> ArrowTableSupport::unregisterArrowTable(
    main::Connection& connection, const std::string& tableName) {
    std::string dropStatement = "DROP TABLE " + tableName;
    return connection.query(dropStatement);
}

} // namespace lbug

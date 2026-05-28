#include <mutex>
#include <string>
#include <unordered_map>

#include "c_api/helpers.h"
#include "c_api/lbug.h"
#include "common/exception/exception.h"
#include "main/lbug.h"
#include "storage/table/arrow_table_support.h"

namespace lbug {
namespace common {
class Value;
}
} // namespace lbug

using namespace lbug::common;
using namespace lbug::main;

static std::mutex arrowTableIDMutex;
static std::unordered_map<Connection*, std::unordered_map<std::string, std::string>> arrowTableIDs;

static void rememberArrowTableID(Connection* connection, const char* tableName,
    std::string arrowId) {
    std::lock_guard<std::mutex> lock(arrowTableIDMutex);
    arrowTableIDs[connection][tableName] = std::move(arrowId);
}

static std::string getRememberedArrowTableID(Connection* connection, const char* tableName) {
    std::lock_guard<std::mutex> lock(arrowTableIDMutex);
    if (!arrowTableIDs.contains(connection) || !arrowTableIDs.at(connection).contains(tableName)) {
        return "";
    }
    return arrowTableIDs.at(connection).at(tableName);
}

static void forgetArrowTableID(Connection* connection, const char* tableName) {
    std::lock_guard<std::mutex> lock(arrowTableIDMutex);
    if (!arrowTableIDs.contains(connection)) {
        return;
    }
    arrowTableIDs.at(connection).erase(tableName);
    if (arrowTableIDs.at(connection).empty()) {
        arrowTableIDs.erase(connection);
    }
}

static void forgetArrowTableIDs(Connection* connection) {
    std::lock_guard<std::mutex> lock(arrowTableIDMutex);
    arrowTableIDs.erase(connection);
}

static ArrowSchemaWrapper takeArrowSchema(ArrowSchema* schema) {
    ArrowSchemaWrapper wrapper;
    static_cast<ArrowSchema&>(wrapper) = *schema;
    schema->release = nullptr;
    return wrapper;
}

static std::vector<ArrowArrayWrapper> takeArrowArrays(ArrowArray* arrays, uint64_t numArrays) {
    std::vector<ArrowArrayWrapper> wrappers;
    wrappers.reserve(numArrays);
    for (auto i = 0u; i < numArrays; ++i) {
        ArrowArrayWrapper wrapper;
        static_cast<ArrowArray&>(wrapper) = arrays[i];
        arrays[i].release = nullptr;
        wrappers.push_back(std::move(wrapper));
    }
    return wrappers;
}

static lbug_state setQueryResult(std::unique_ptr<QueryResult> queryResult,
    lbug_query_result* outQueryResult) {
    if (queryResult == nullptr) {
        return LbugError;
    }
    auto queryResultPtr = queryResult.release();
    outQueryResult->_query_result = queryResultPtr;
    outQueryResult->_is_owned_by_cpp = false;
    if (!queryResultPtr->isSuccess()) {
        return LbugError;
    }
    return LbugSuccess;
}

lbug_state lbug_connection_init(lbug_database* database, lbug_connection* out_connection) {
    if (database == nullptr || database->_database == nullptr) {
        out_connection->_connection = nullptr;
        return LbugError;
    }
    try {
        out_connection->_connection = new Connection(static_cast<Database*>(database->_database));
    } catch (Exception& e) {
        out_connection->_connection = nullptr;
        return LbugError;
    }
    return LbugSuccess;
}

void lbug_connection_destroy(lbug_connection* connection) {
    if (connection == nullptr) {
        return;
    }
    if (connection->_connection != nullptr) {
        auto connectionPtr = static_cast<Connection*>(connection->_connection);
        forgetArrowTableIDs(connectionPtr);
        delete connectionPtr;
        connection->_connection = nullptr;
    }
}

lbug_state lbug_connection_set_max_num_thread_for_exec(lbug_connection* connection,
    uint64_t num_threads) {
    if (connection == nullptr || connection->_connection == nullptr) {
        return LbugError;
    }
    try {
        static_cast<Connection*>(connection->_connection)->setMaxNumThreadForExec(num_threads);
    } catch (Exception& e) {
        return LbugError;
    }
    return LbugSuccess;
}

lbug_state lbug_connection_get_max_num_thread_for_exec(lbug_connection* connection,
    uint64_t* out_result) {
    if (connection == nullptr || connection->_connection == nullptr) {
        return LbugError;
    }
    try {
        *out_result = static_cast<Connection*>(connection->_connection)->getMaxNumThreadForExec();
    } catch (Exception& e) {
        return LbugError;
    }
    return LbugSuccess;
}

lbug_state lbug_connection_query(lbug_connection* connection, const char* query,
    lbug_query_result* out_query_result) {
    if (connection == nullptr || connection->_connection == nullptr) {
        return LbugError;
    }
    try {
        clearLastCAPIErrorMessage();
        auto query_result =
            static_cast<Connection*>(connection->_connection)->query(query).release();
        if (query_result == nullptr) {
            return LbugError;
        }
        out_query_result->_query_result = query_result;
        out_query_result->_is_owned_by_cpp = false;
        if (!query_result->isSuccess()) {
            return LbugError;
        }
        return LbugSuccess;
    } catch (Exception& e) {
        setLastCAPIErrorMessage(e.what());
        return LbugError;
    }
}

lbug_state lbug_connection_prepare(lbug_connection* connection, const char* query,
    lbug_prepared_statement* out_prepared_statement) {
    if (connection == nullptr || connection->_connection == nullptr) {
        return LbugError;
    }
    try {
        clearLastCAPIErrorMessage();
        auto prepared_statement =
            static_cast<Connection*>(connection->_connection)->prepare(query).release();
        if (prepared_statement == nullptr) {
            return LbugError;
        }
        out_prepared_statement->_prepared_statement = prepared_statement;
        out_prepared_statement->_bound_values =
            new std::unordered_map<std::string, std::unique_ptr<Value>>;
        return LbugSuccess;
    } catch (Exception& e) {
        setLastCAPIErrorMessage(e.what());
        return LbugError;
    }
    return LbugSuccess;
}

lbug_state lbug_connection_execute(lbug_connection* connection,
    lbug_prepared_statement* prepared_statement, lbug_query_result* out_query_result) {
    if (connection == nullptr || connection->_connection == nullptr ||
        prepared_statement == nullptr || prepared_statement->_prepared_statement == nullptr ||
        prepared_statement->_bound_values == nullptr) {
        return LbugError;
    }
    try {
        clearLastCAPIErrorMessage();
        auto prepared_statement_ptr =
            static_cast<PreparedStatement*>(prepared_statement->_prepared_statement);
        auto bound_values = static_cast<std::unordered_map<std::string, std::unique_ptr<Value>>*>(
            prepared_statement->_bound_values);

        // Must copy the parameters for safety, and so that the parameters in the prepared statement
        // stay the same.
        std::unordered_map<std::string, std::unique_ptr<Value>> copied_bound_values;
        for (auto& [name, value] : *bound_values) {
            copied_bound_values.emplace(name, value->copy());
        }

        auto query_result =
            static_cast<Connection*>(connection->_connection)
                ->executeWithParams(prepared_statement_ptr, std::move(copied_bound_values))
                .release();
        if (query_result == nullptr) {
            return LbugError;
        }
        out_query_result->_query_result = query_result;
        out_query_result->_is_owned_by_cpp = false;
        if (!query_result->isSuccess()) {
            return LbugError;
        }
        return LbugSuccess;
    } catch (Exception& e) {
        setLastCAPIErrorMessage(e.what());
        return LbugError;
    }
}

lbug_state lbug_connection_create_arrow_table(lbug_connection* connection, const char* table_name,
    ArrowSchema* schema, ArrowArray* arrays, uint64_t num_arrays,
    lbug_query_result* out_query_result) {
    if (connection == nullptr || connection->_connection == nullptr || table_name == nullptr ||
        schema == nullptr || arrays == nullptr || out_query_result == nullptr) {
        return LbugError;
    }
    try {
        clearLastCAPIErrorMessage();
        auto result = lbug::ArrowTableSupport::createViewFromArrowTable(
            *static_cast<Connection*>(connection->_connection), table_name, takeArrowSchema(schema),
            takeArrowArrays(arrays, num_arrays));
        auto state = setQueryResult(std::move(result.queryResult), out_query_result);
        if (state == LbugSuccess) {
            rememberArrowTableID(static_cast<Connection*>(connection->_connection), table_name,
                std::move(result.arrowId));
        }
        return state;
    } catch (Exception& e) {
        setLastCAPIErrorMessage(e.what());
        return LbugError;
    }
}

lbug_state lbug_connection_create_arrow_rel_table(lbug_connection* connection,
    const char* table_name, const char* src_table_name, const char* dst_table_name,
    ArrowSchema* schema, ArrowArray* arrays, uint64_t num_arrays,
    lbug_query_result* out_query_result) {
    if (connection == nullptr || connection->_connection == nullptr || table_name == nullptr ||
        src_table_name == nullptr || dst_table_name == nullptr || schema == nullptr ||
        arrays == nullptr || out_query_result == nullptr) {
        return LbugError;
    }
    try {
        clearLastCAPIErrorMessage();
        auto result = lbug::ArrowTableSupport::createRelTableFromArrowTable(
            *static_cast<Connection*>(connection->_connection), table_name, src_table_name,
            dst_table_name, takeArrowSchema(schema), takeArrowArrays(arrays, num_arrays));
        auto state = setQueryResult(std::move(result.queryResult), out_query_result);
        if (state == LbugSuccess) {
            rememberArrowTableID(static_cast<Connection*>(connection->_connection), table_name,
                std::move(result.arrowId));
        }
        return state;
    } catch (Exception& e) {
        setLastCAPIErrorMessage(e.what());
        return LbugError;
    }
}

lbug_state lbug_connection_create_arrow_rel_table_csr(lbug_connection* connection,
    const char* table_name, const char* src_table_name, const char* dst_table_name,
    ArrowSchema* indices_schema, ArrowArray* indices_arrays, uint64_t num_indices_arrays,
    ArrowSchema* indptr_schema, ArrowArray* indptr_arrays, uint64_t num_indptr_arrays,
    const char* dst_col_name, lbug_query_result* out_query_result) {
    if (connection == nullptr || connection->_connection == nullptr || table_name == nullptr ||
        src_table_name == nullptr || dst_table_name == nullptr || indices_schema == nullptr ||
        indices_arrays == nullptr || indptr_schema == nullptr || indptr_arrays == nullptr ||
        out_query_result == nullptr) {
        return LbugError;
    }
    try {
        clearLastCAPIErrorMessage();
        auto result = lbug::ArrowTableSupport::createRelTableFromArrowCSR(
            *static_cast<Connection*>(connection->_connection), table_name, src_table_name,
            dst_table_name, takeArrowSchema(indices_schema),
            takeArrowArrays(indices_arrays, num_indices_arrays), takeArrowSchema(indptr_schema),
            takeArrowArrays(indptr_arrays, num_indptr_arrays),
            dst_col_name != nullptr ? dst_col_name : "to");
        auto state = setQueryResult(std::move(result.queryResult), out_query_result);
        if (state == LbugSuccess) {
            rememberArrowTableID(static_cast<Connection*>(connection->_connection), table_name,
                std::move(result.arrowId));
        }
        return state;
    } catch (Exception& e) {
        setLastCAPIErrorMessage(e.what());
        return LbugError;
    }
}

lbug_state lbug_connection_drop_arrow_table(lbug_connection* connection, const char* table_name,
    lbug_query_result* out_query_result) {
    if (connection == nullptr || connection->_connection == nullptr || table_name == nullptr ||
        out_query_result == nullptr) {
        return LbugError;
    }
    try {
        clearLastCAPIErrorMessage();
        auto connectionPtr = static_cast<Connection*>(connection->_connection);
        auto arrowId = getRememberedArrowTableID(connectionPtr, table_name);
        auto result = lbug::ArrowTableSupport::unregisterArrowTable(*connectionPtr, table_name);
        auto state = setQueryResult(std::move(result), out_query_result);
        if (state == LbugSuccess) {
            if (!arrowId.empty()) {
                lbug::ArrowTableSupport::unregisterArrowData(arrowId);
            }
            forgetArrowTableID(connectionPtr, table_name);
        }
        return state;
    } catch (Exception& e) {
        setLastCAPIErrorMessage(e.what());
        return LbugError;
    }
}

void lbug_connection_interrupt(lbug_connection* connection) {
    static_cast<Connection*>(connection->_connection)->interrupt();
}

lbug_state lbug_connection_set_query_timeout(lbug_connection* connection, uint64_t timeout_in_ms) {
    if (connection == nullptr || connection->_connection == nullptr) {
        return LbugError;
    }
    try {
        static_cast<Connection*>(connection->_connection)->setQueryTimeOut(timeout_in_ms);
    } catch (Exception& e) {
        return LbugError;
    }
    return LbugSuccess;
}

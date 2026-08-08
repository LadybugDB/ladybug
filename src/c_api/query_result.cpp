#include "main/query_result.h"

#include "c_api/helpers.h"
#include "c_api/lbug.h"

using namespace lbug::main;
using namespace lbug::common;
using namespace lbug::processor;

void lbug_query_result_destroy(lbug_query_result* query_result) {
    LBUG_C_API_GUARD_BEGIN
    if (query_result == nullptr) {
        return;
    }
    if (query_result->_query_result != nullptr) {
        if (!query_result->_is_owned_by_cpp) {
            delete static_cast<QueryResult*>(query_result->_query_result);
        }
        query_result->_query_result = nullptr;
    }
    LBUG_C_API_GUARD_END_VOID
}

bool lbug_query_result_is_success(lbug_query_result* query_result) {
    if (query_result == nullptr || query_result->_query_result == nullptr) {
        return false;
    }
    LBUG_C_API_GUARD_BEGIN
    return static_cast<QueryResult*>(query_result->_query_result)->isSuccess();
    LBUG_C_API_GUARD_END(false)
}

char* lbug_query_result_get_error_message(lbug_query_result* query_result) {
    if (query_result == nullptr || query_result->_query_result == nullptr) {
        return nullptr;
    }
    LBUG_C_API_GUARD_BEGIN
    auto error_message = static_cast<QueryResult*>(query_result->_query_result)->getErrorMessage();
    if (error_message.empty()) {
        return nullptr;
    }
    return convertToOwnedCString(error_message);
    LBUG_C_API_GUARD_END(nullptr)
}

uint64_t lbug_query_result_get_num_columns(lbug_query_result* query_result) {
    if (query_result == nullptr || query_result->_query_result == nullptr) {
        return 0;
    }
    LBUG_C_API_GUARD_BEGIN
    return static_cast<QueryResult*>(query_result->_query_result)->getNumColumns();
    LBUG_C_API_GUARD_END(0)
}

lbug_state lbug_query_result_get_column_name(lbug_query_result* query_result, uint64_t index,
    char** out_column_name) {
    if (query_result == nullptr || query_result->_query_result == nullptr) {
        return LbugError;
    }
    LBUG_C_API_GUARD_BEGIN
    auto column_names = static_cast<QueryResult*>(query_result->_query_result)->getColumnNames();
    if (index >= column_names.size()) {
        return LbugError;
    }
    *out_column_name = convertToOwnedCString(column_names[index]);
    return LbugSuccess;
    LBUG_C_API_GUARD_END(LbugError)
}

lbug_state lbug_query_result_get_column_data_type(lbug_query_result* query_result, uint64_t index,
    lbug_logical_type* out_column_data_type) {
    if (query_result == nullptr || query_result->_query_result == nullptr) {
        return LbugError;
    }
    LBUG_C_API_GUARD_BEGIN
    auto column_data_types =
        static_cast<QueryResult*>(query_result->_query_result)->getColumnDataTypes();
    if (index >= column_data_types.size()) {
        return LbugError;
    }
    const auto& column_data_type = column_data_types[index];
    out_column_data_type->_data_type = new LogicalType(column_data_type.copy());
    return LbugSuccess;
    LBUG_C_API_GUARD_END(LbugError)
}

uint64_t lbug_query_result_get_num_tuples(lbug_query_result* query_result) {
    if (query_result == nullptr || query_result->_query_result == nullptr) {
        return 0;
    }
    LBUG_C_API_GUARD_BEGIN
    return static_cast<QueryResult*>(query_result->_query_result)->getNumTuples();
    LBUG_C_API_GUARD_END(0)
}

lbug_state lbug_query_result_get_query_summary(lbug_query_result* query_result,
    lbug_query_summary* out_query_summary) {
    if (query_result == nullptr || query_result->_query_result == nullptr) {
        return LbugError;
    }
    LBUG_C_API_GUARD_BEGIN
    if (out_query_summary == nullptr) {
        return LbugError;
    }
    auto query_summary = static_cast<QueryResult*>(query_result->_query_result)->getQuerySummary();
    out_query_summary->_query_summary = query_summary;
    return LbugSuccess;
    LBUG_C_API_GUARD_END(LbugError)
}

bool lbug_query_result_has_next(lbug_query_result* query_result) {
    if (query_result == nullptr || query_result->_query_result == nullptr) {
        return false;
    }
    LBUG_C_API_GUARD_BEGIN
    return static_cast<QueryResult*>(query_result->_query_result)->hasNext();
    LBUG_C_API_GUARD_END(false)
}

bool lbug_query_result_has_next_query_result(lbug_query_result* query_result) {
    if (query_result == nullptr || query_result->_query_result == nullptr) {
        return false;
    }
    LBUG_C_API_GUARD_BEGIN
    return static_cast<QueryResult*>(query_result->_query_result)->hasNextQueryResult();
    LBUG_C_API_GUARD_END(false)
}

lbug_state lbug_query_result_get_next_query_result(lbug_query_result* query_result,
    lbug_query_result* out_query_result) {
    if (query_result == nullptr || query_result->_query_result == nullptr) {
        return LbugError;
    }
    LBUG_C_API_GUARD_BEGIN
    if (!lbug_query_result_has_next_query_result(query_result)) {
        return LbugError;
    }
    auto next_query_result =
        static_cast<QueryResult*>(query_result->_query_result)->getNextQueryResult();
    if (next_query_result == nullptr) {
        return LbugError;
    }
    out_query_result->_query_result = next_query_result;
    out_query_result->_is_owned_by_cpp = true;
    return LbugSuccess;
    LBUG_C_API_GUARD_END(LbugError)
}

lbug_state lbug_query_result_get_next(lbug_query_result* query_result,
    lbug_flat_tuple* out_flat_tuple) {
    if (query_result == nullptr || query_result->_query_result == nullptr) {
        return LbugError;
    }
    LBUG_C_API_GUARD_BEGIN
    try {
        clearLastCAPIErrorMessage();
        auto flat_tuple = static_cast<QueryResult*>(query_result->_query_result)->getNext();
        out_flat_tuple->_flat_tuple = flat_tuple.get();
        out_flat_tuple->_is_owned_by_cpp = true;
        return LbugSuccess;
    } catch (Exception& e) {
        setLastCAPIErrorMessage(e.what());
        return LbugError;
    }
    LBUG_C_API_GUARD_END(LbugError)
}

char* lbug_query_result_to_string(lbug_query_result* query_result) {
    if (query_result == nullptr || query_result->_query_result == nullptr) {
        return nullptr;
    }
    LBUG_C_API_GUARD_BEGIN
    std::string result_string = static_cast<QueryResult*>(query_result->_query_result)->toString();
    return convertToOwnedCString(result_string);
    LBUG_C_API_GUARD_END(nullptr)
}

void lbug_query_result_reset_iterator(lbug_query_result* query_result) {
    if (query_result == nullptr || query_result->_query_result == nullptr) {
        return;
    }
    LBUG_C_API_GUARD_BEGIN
    static_cast<QueryResult*>(query_result->_query_result)->resetIterator();
    LBUG_C_API_GUARD_END_VOID
}

lbug_state lbug_query_result_get_arrow_schema(lbug_query_result* query_result,
    ArrowSchema* out_schema) {
    if (query_result == nullptr || query_result->_query_result == nullptr) {
        return LbugError;
    }
    LBUG_C_API_GUARD_BEGIN
    try {
        *out_schema = *static_cast<QueryResult*>(query_result->_query_result)->getArrowSchema();
        return LbugSuccess;
    } catch (Exception& e) {
        setLastCAPIErrorMessage(e.what());
        return LbugError;
    }
    LBUG_C_API_GUARD_END(LbugError)
}

lbug_state lbug_query_result_get_next_arrow_chunk(lbug_query_result* query_result,
    int64_t chunk_size, ArrowArray* out_arrow_array) {
    if (query_result == nullptr || query_result->_query_result == nullptr) {
        return LbugError;
    }
    LBUG_C_API_GUARD_BEGIN
    try {
        *out_arrow_array =
            *static_cast<QueryResult*>(query_result->_query_result)->getNextArrowChunk(chunk_size);
        return LbugSuccess;
    } catch (Exception& e) {
        setLastCAPIErrorMessage(e.what());
        return LbugError;
    }
    LBUG_C_API_GUARD_END(LbugError)
}

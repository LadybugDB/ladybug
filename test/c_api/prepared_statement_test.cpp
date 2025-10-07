#include "c_api_test/c_api_test.h"

using namespace lbug::main;
using namespace lbug::testing;

class CApiPreparedStatementTest : public CApiTest {
public:
    std::string getInputDir() override {
        return TestHelper::appendLbugRootPath("dataset/tinysnb/");
    }
};

TEST_F(CApiPreparedStatementTest, IsSuccess) {
    lbug_prepared_statement preparedStatement;
    lbug_state state;
    auto connection = getConnection();
    auto query = "MATCH (a:person) WHERE a.isStudent = $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(preparedStatement._prepared_statement, nullptr);
    ASSERT_TRUE(lbug_prepared_statement_is_success(&preparedStatement));
    lbug_prepared_statement_destroy(&preparedStatement);

    query = "MATCH (a:personnnn) WHERE a.isStudent = $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(preparedStatement._prepared_statement, nullptr);
    ASSERT_FALSE(lbug_prepared_statement_is_success(&preparedStatement));
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, GetErrorMessage) {
    lbug_prepared_statement preparedStatement;
    lbug_state state;
    auto connection = getConnection();
    auto query = "MATCH (a:person) WHERE a.isStudent = $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(preparedStatement._prepared_statement, nullptr);
    ASSERT_EQ(lbug_prepared_statement_get_error_message(&preparedStatement), nullptr);
    lbug_prepared_statement_destroy(&preparedStatement);

    query = "MATCH (a:personnnn) WHERE a.isStudent = $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(preparedStatement._prepared_statement, nullptr);
    char* message = lbug_prepared_statement_get_error_message(&preparedStatement);
    ASSERT_EQ(std::string(message), "Binder exception: Table personnnn does not exist.");
    lbug_prepared_statement_destroy(&preparedStatement);
    lbug_destroy_string(message);
}

TEST_F(CApiPreparedStatementTest, BindBool) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query = "MATCH (a:person) WHERE a.isStudent = $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_bool(&preparedStatement, "1", true), LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 3);
    lbug_query_result_destroy(&result);
    // Bind a different parameter
    ASSERT_EQ(lbug_prepared_statement_bind_bool(&preparedStatement, "1", false), LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    resultCpp = static_cast<QueryResult*>(result._query_result);
    tuple = resultCpp->getNext();
    value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 5);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, BindInt64) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query = "MATCH (a:person) WHERE a.age > $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_int64(&preparedStatement, "1", 30), LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 4);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, BindInt32) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query = "MATCH (a:movies) WHERE a.length > $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_int32(&preparedStatement, "1", 200), LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 2);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, BindInt16) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query =
        "MATCH (a:person) -[s:studyAt]-> (b:organisation) WHERE s.length > $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_int16(&preparedStatement, "1", 10), LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 2);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, BindInt8) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query =
        "MATCH (a:person) -[s:studyAt]-> (b:organisation) WHERE s.level > $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_int8(&preparedStatement, "1", 3), LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 2);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, BindUInt64) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query =
        "MATCH (a:person) -[s:studyAt]-> (b:organisation) WHERE s.code > $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(lbug_prepared_statement_bind_uint64(&preparedStatement, "1", 100), LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 2);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, BindUInt32) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query =
        "MATCH (a:person) -[s:studyAt]-> (b:organisation) WHERE s.temperature> $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_uint32(&preparedStatement, "1", 10), LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 2);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, BindUInt16) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query =
        "MATCH (a:person) -[s:studyAt]-> (b:organisation) WHERE s.ulength> $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_uint16(&preparedStatement, "1", 100), LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 2);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, BindUInt8) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query =
        "MATCH (a:person) -[s:studyAt]-> (b:organisation) WHERE s.ulevel> $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_uint8(&preparedStatement, "1", 14), LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 2);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, BindDouble) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query = "MATCH (a:person) WHERE a.eyeSight > $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_double(&preparedStatement, "1", 4.5), LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 7);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, BindFloat) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query = "MATCH (a:person) WHERE a.height < $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_float(&preparedStatement, "1", 1.0), LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 1);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, BindString) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query = "MATCH (a:person) WHERE a.fName = $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_prepared_statement_is_success(&preparedStatement));
    ASSERT_EQ(lbug_prepared_statement_bind_string(&preparedStatement, "1", "Alice"), LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 1);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, BindDate) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query = "MATCH (a:person) WHERE a.birthdate > $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_prepared_statement_is_success(&preparedStatement));
    auto date = lbug_date_t{0};
    ASSERT_EQ(lbug_prepared_statement_bind_date(&preparedStatement, "1", date), LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 4);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, BindTimestamp) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query = "MATCH (a:person) WHERE a.registerTime > $1 and cast(a.registerTime, "
                 "\"timestamp_ns\") > $2 and cast(a.registerTime, \"timestamp_ms\") > "
                 "$3 and cast(a.registerTime, \"timestamp_sec\") > $4 and cast(a.registerTime, "
                 "\"timestamp_tz\") > $5 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_prepared_statement_is_success(&preparedStatement));
    auto timestamp = lbug_timestamp_t{0};
    auto timestamp_ns = lbug_timestamp_ns_t{1};
    auto timestamp_ms = lbug_timestamp_ms_t{2};
    auto timestamp_sec = lbug_timestamp_sec_t{3};
    auto timestamp_tz = lbug_timestamp_tz_t{4};
    ASSERT_EQ(lbug_prepared_statement_bind_timestamp(&preparedStatement, "1", timestamp),
        LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_timestamp_ns(&preparedStatement, "2", timestamp_ns),
        LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_timestamp_ms(&preparedStatement, "3", timestamp_ms),
        LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_timestamp_sec(&preparedStatement, "4", timestamp_sec),
        LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_timestamp_tz(&preparedStatement, "5", timestamp_tz),
        LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 7);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, BindInteval) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query = "MATCH (a:person) WHERE a.lastJobDuration > $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_prepared_statement_is_success(&preparedStatement));
    auto interval = lbug_interval_t{0, 0, 0};
    ASSERT_EQ(lbug_prepared_statement_bind_interval(&preparedStatement, "1", interval),
        LbugSuccess);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 8);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

TEST_F(CApiPreparedStatementTest, BindValue) {
    lbug_prepared_statement preparedStatement;
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    auto query = "MATCH (a:person) WHERE a.registerTime > $1 RETURN COUNT(*)";
    state = lbug_connection_prepare(connection, query, &preparedStatement);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_prepared_statement_is_success(&preparedStatement));
    auto timestamp = lbug_timestamp_t{0};
    auto timestampValue = lbug_value_create_timestamp(timestamp);
    ASSERT_EQ(lbug_prepared_statement_bind_value(&preparedStatement, "1", timestampValue),
        LbugSuccess);
    lbug_value_destroy(timestampValue);
    state = lbug_connection_execute(connection, &preparedStatement, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_NE(result._query_result, nullptr);
    ASSERT_EQ(lbug_query_result_get_num_tuples(&result), 1);
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 1);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    auto resultCpp = static_cast<QueryResult*>(result._query_result);
    auto tuple = resultCpp->getNext();
    auto value = tuple->getValue(0)->getValue<int64_t>();
    ASSERT_EQ(value, 7);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&preparedStatement);
}

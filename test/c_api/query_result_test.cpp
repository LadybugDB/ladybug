#include <fstream>

#include "c_api_test/c_api_test.h"

using namespace lbug::main;
using namespace lbug::common;
using namespace lbug::processor;
using namespace lbug::testing;

class CApiQueryResultTest : public CApiTest {
public:
    std::string getInputDir() override {
        return TestHelper::appendLbugRootPath("dataset/tinysnb/");
    }
};

static lbug_value* copy_flat_tuple(lbug_flat_tuple* tuple, uint32_t tupleLen) {
    lbug_value* ret = (lbug_value*)malloc(sizeof(lbug_value) * tupleLen);
    for (uint32_t i = 0; i < tupleLen; i++) {
        lbug_flat_tuple_get_value(tuple, i, &ret[i]);
    }
    return ret;
}

TEST_F(CApiQueryResultTest, GetNextExample) {
    auto conn = getConnection();

    lbug_query_result result;
    lbug_connection_query(conn, "MATCH (p:person) RETURN p.*", &result);

    uint64_t num_tuples = lbug_query_result_get_num_tuples(&result);
    lbug_value** tuples = (lbug_value**)malloc(sizeof(lbug_value*) * num_tuples);
    for (uint64_t i = 0; i < num_tuples; ++i) {
        lbug_flat_tuple tuple;
        lbug_query_result_get_next(&result, &tuple);
        tuples[i] = copy_flat_tuple(&tuple, lbug_query_result_get_num_columns(&result));
        lbug_flat_tuple_destroy(&tuple);
    }

    for (uint64_t i = 0; i < num_tuples; ++i) {
        for (uint64_t j = 0; j < lbug_query_result_get_num_columns(&result); ++j) {
            ASSERT_FALSE(lbug_value_is_null(&tuples[i][j]));
            lbug_value_destroy(&tuples[i][j]);
        }
        free(tuples[i]);
    }

    free((void*)tuples);

    lbug_query_result_destroy(&result);
}

TEST_F(CApiQueryResultTest, GetErrorMessage) {
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection, "MATCH (a:person) RETURN COUNT(*)", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    char* errorMessage = lbug_query_result_get_error_message(&result);
    lbug_query_result_destroy(&result);

    state = lbug_connection_query(connection, "MATCH (a:personnnn) RETURN COUNT(*)", &result);
    ASSERT_EQ(state, LbugError);
    ASSERT_FALSE(lbug_query_result_is_success(&result));
    errorMessage = lbug_query_result_get_error_message(&result);
    ASSERT_EQ(std::string(errorMessage), "Binder exception: Table personnnn does not exist.");
    lbug_query_result_destroy(&result);
    lbug_destroy_string(errorMessage);
}

TEST_F(CApiQueryResultTest, ToString) {
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection, "MATCH (a:person) RETURN COUNT(*)", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    char* str_repr = lbug_query_result_to_string(&result);
    ASSERT_EQ(state, LbugSuccess);
    lbug_destroy_string(str_repr);
    lbug_query_result_destroy(&result);
}

TEST_F(CApiQueryResultTest, GetNumColumns) {
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection, "MATCH (a:person) RETURN a.fName, a.age, a.height",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_EQ(lbug_query_result_get_num_columns(&result), 3);
    lbug_query_result_destroy(&result);
}

TEST_F(CApiQueryResultTest, GetColumnName) {
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection, "MATCH (a:person) RETURN a.fName, a.age, a.height",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    char* columnName;
    ASSERT_EQ(lbug_query_result_get_column_name(&result, 0, &columnName), LbugSuccess);
    ASSERT_EQ(std::string(columnName), "a.fName");
    lbug_destroy_string(columnName);
    ASSERT_EQ(lbug_query_result_get_column_name(&result, 1, &columnName), LbugSuccess);
    ASSERT_EQ(std::string(columnName), "a.age");
    lbug_destroy_string(columnName);
    ASSERT_EQ(lbug_query_result_get_column_name(&result, 2, &columnName), LbugSuccess);
    ASSERT_EQ(std::string(columnName), "a.height");
    lbug_destroy_string(columnName);
    ASSERT_EQ(lbug_query_result_get_column_name(&result, 222, &columnName), LbugError);
    lbug_query_result_destroy(&result);
}

TEST_F(CApiQueryResultTest, GetColumnDataType) {
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection, "MATCH (a:person) RETURN a.fName, a.age, a.height",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    lbug_logical_type type;
    ASSERT_EQ(lbug_query_result_get_column_data_type(&result, 0, &type), LbugSuccess);
    auto typeCpp = (LogicalType*)(type._data_type);
    ASSERT_EQ(typeCpp->getLogicalTypeID(), LogicalTypeID::STRING);
    lbug_data_type_destroy(&type);
    ASSERT_EQ(lbug_query_result_get_column_data_type(&result, 1, &type), LbugSuccess);
    typeCpp = (LogicalType*)(type._data_type);
    ASSERT_EQ(typeCpp->getLogicalTypeID(), LogicalTypeID::INT64);
    lbug_data_type_destroy(&type);
    ASSERT_EQ(lbug_query_result_get_column_data_type(&result, 2, &type), LbugSuccess);
    typeCpp = (LogicalType*)(type._data_type);
    ASSERT_EQ(typeCpp->getLogicalTypeID(), LogicalTypeID::FLOAT);
    lbug_data_type_destroy(&type);
    ASSERT_EQ(lbug_query_result_get_column_data_type(&result, 222, &type), LbugError);
    lbug_query_result_destroy(&result);
}

// TODO(Guodong): Fix this test by adding support of STRUCT in arrow table export.
// TEST_F(CApiQueryResultTest, GetArrowSchema) {
//    auto connection = getConnection();
//    auto result = lbug_connection_query(
//        connection, "MATCH (p:person)-[k:knows]-(q:person) RETURN p.fName, k, q.fName");
//    ASSERT_TRUE(lbug_query_result_is_success(result));
//    auto schema = lbug_query_result_get_arrow_schema(result);
//    ASSERT_STREQ(schema.name, "lbug_query_result");
//    ASSERT_EQ(schema.n_children, 3);
//    ASSERT_STREQ(schema.children[0]->name, "p.fName");
//    ASSERT_STREQ(schema.children[1]->name, "k");
//    ASSERT_STREQ(schema.children[2]->name, "q.fName");
//
//    schema.release(&schema);
//    lbug_query_result_destroy(result);
//}

TEST_F(CApiQueryResultTest, GetQuerySummary) {
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection, "MATCH (a:person) RETURN a.fName, a.age, a.height",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    lbug_query_summary summary;
    state = lbug_query_result_get_query_summary(&result, &summary);
    ASSERT_EQ(state, LbugSuccess);
    auto compilingTime = lbug_query_summary_get_compiling_time(&summary);
    ASSERT_GT(compilingTime, 0);
    auto executionTime = lbug_query_summary_get_execution_time(&summary);
    ASSERT_GT(executionTime, 0);
    lbug_query_summary_destroy(&summary);
    lbug_query_result_destroy(&result);
}

TEST_F(CApiQueryResultTest, GetNext) {
    lbug_query_result result;
    lbug_flat_tuple row;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        "MATCH (a:person) RETURN a.fName, a.age ORDER BY a.fName", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));

    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &row);
    ASSERT_EQ(state, LbugSuccess);
    auto flatTupleCpp = (FlatTuple*)(row._flat_tuple);
    ASSERT_EQ(flatTupleCpp->getValue(0)->getValue<std::string>(), "Alice");
    ASSERT_EQ(flatTupleCpp->getValue(1)->getValue<int64_t>(), 35);

    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &row);
    ASSERT_EQ(state, LbugSuccess);
    flatTupleCpp = (FlatTuple*)(row._flat_tuple);
    ASSERT_EQ(flatTupleCpp->getValue(0)->getValue<std::string>(), "Bob");
    ASSERT_EQ(flatTupleCpp->getValue(1)->getValue<int64_t>(), 30);
    lbug_flat_tuple_destroy(&row);

    while (lbug_query_result_has_next(&result)) {
        lbug_query_result_get_next(&result, &row);
    }
    ASSERT_FALSE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &row);
    ASSERT_EQ(state, LbugError);
    lbug_query_result_destroy(&result);
}

TEST_F(CApiQueryResultTest, ResetIterator) {
    lbug_query_result result;
    lbug_flat_tuple row;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        "MATCH (a:person) RETURN a.fName, a.age ORDER BY a.fName", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));

    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &row);
    ASSERT_EQ(state, LbugSuccess);
    auto flatTupleCpp = (FlatTuple*)(row._flat_tuple);
    ASSERT_EQ(flatTupleCpp->getValue(0)->getValue<std::string>(), "Alice");
    ASSERT_EQ(flatTupleCpp->getValue(1)->getValue<int64_t>(), 35);

    lbug_query_result_reset_iterator(&result);

    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &row);
    ASSERT_EQ(state, LbugSuccess);
    flatTupleCpp = (FlatTuple*)(row._flat_tuple);
    ASSERT_EQ(flatTupleCpp->getValue(0)->getValue<std::string>(), "Alice");
    ASSERT_EQ(flatTupleCpp->getValue(1)->getValue<int64_t>(), 35);
    lbug_flat_tuple_destroy(&row);

    lbug_query_result_destroy(&result);
}

TEST_F(CApiQueryResultTest, MultipleQuery) {
    lbug_query_result result;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection, "return 1; return 2; return 3;", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));

    char* str = lbug_query_result_to_string(&result);
    ASSERT_EQ(std::string(str), "1\n1\n");
    lbug_destroy_string(str);

    ASSERT_TRUE(lbug_query_result_has_next_query_result(&result));
    lbug_query_result next_query_result;
    ASSERT_EQ(lbug_query_result_get_next_query_result(&result, &next_query_result), LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&next_query_result));
    str = lbug_query_result_to_string(&next_query_result);
    ASSERT_EQ(std::string(str), "2\n2\n");
    lbug_destroy_string(str);
    lbug_query_result_destroy(&next_query_result);

    ASSERT_EQ(lbug_query_result_get_next_query_result(&result, &next_query_result), LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&next_query_result));
    str = lbug_query_result_to_string(&next_query_result);
    ASSERT_EQ(std::string(str), "3\n3\n");
    lbug_destroy_string(str);

    ASSERT_FALSE(lbug_query_result_has_next_query_result(&result));
    ASSERT_EQ(lbug_query_result_get_next_query_result(&result, &next_query_result), LbugError);
    lbug_query_result_destroy(&next_query_result);

    lbug_query_result_destroy(&result);
}

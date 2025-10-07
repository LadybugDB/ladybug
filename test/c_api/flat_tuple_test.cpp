#include "c_api_test/c_api_test.h"

using namespace lbug::common;
using namespace lbug::main;
using namespace lbug::testing;

class CApiFlatTupleTest : public CApiTest {
public:
    std::string getInputDir() override {
        return TestHelper::appendLbugRootPath("dataset/tinysnb/");
    }
};

TEST_F(CApiFlatTupleTest, GetValue) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        "MATCH (a:person) RETURN a.fName, a.age, a.height ORDER BY a.fName LIMIT 1", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_NE(value._value, nullptr);
    auto valueCpp = static_cast<Value*>(value._value);
    ASSERT_NE(valueCpp, nullptr);
    ASSERT_EQ(valueCpp->getDataType().getLogicalTypeID(), LogicalTypeID::STRING);
    ASSERT_EQ(valueCpp->getValue<std::string>(), "Alice");
    lbug_value_destroy(&value);
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 1, &value), LbugSuccess);
    ASSERT_NE(value._value, nullptr);
    valueCpp = static_cast<Value*>(value._value);
    ASSERT_NE(valueCpp, nullptr);
    ASSERT_EQ(valueCpp->getDataType().getLogicalTypeID(), LogicalTypeID::INT64);
    ASSERT_EQ(valueCpp->getValue<int64_t>(), 35);
    lbug_value_destroy(&value);
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 2, &value), LbugSuccess);
    ASSERT_NE(value._value, nullptr);
    valueCpp = static_cast<Value*>(value._value);
    ASSERT_NE(valueCpp, nullptr);
    ASSERT_EQ(valueCpp->getDataType().getLogicalTypeID(), LogicalTypeID::FLOAT);
    ASSERT_FLOAT_EQ(valueCpp->getValue<float>(), 1.731);
    lbug_value_destroy(&value);
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 222, &value), LbugError);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);
}

TEST_F(CApiFlatTupleTest, ToString) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        "MATCH (a:person) RETURN a.fName, a.age, a.height ORDER BY a.fName LIMIT 1", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    auto columnWidths = (uint32_t*)malloc(3 * sizeof(uint32_t));
    columnWidths[0] = 10;
    columnWidths[1] = 5;
    columnWidths[2] = 10;
    char* str = lbug_flat_tuple_to_string(&flatTuple);
    ASSERT_EQ(std::string(str), "Alice|35|1.731000\n");
    lbug_destroy_string(str);
    free(columnWidths);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);
}

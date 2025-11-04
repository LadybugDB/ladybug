#include "c_api_test/c_api_test.h"

using namespace lbug::main;
using namespace lbug::common;
using namespace lbug::testing;

class CApiValueTest : public CApiTest {
public:
    std::string getInputDir() override {
        return TestHelper::appendLbugRootPath("dataset/tinysnb/");
    }
};

TEST(CApiValueTestEmptyDB, CreateNull) {
    lbug_value* value = lbug_value_create_null();
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::ANY);
    ASSERT_EQ(cppValue->isNull(), true);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateNullWithDatatype) {
    lbug_logical_type type;
    lbug_data_type_create(lbug_data_type_id::LBUG_INT64, nullptr, 0, &type);
    lbug_value* value = lbug_value_create_null_with_data_type(&type);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    lbug_data_type_destroy(&type);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::INT64);
    ASSERT_EQ(cppValue->isNull(), true);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, IsNull) {
    lbug_value* value = lbug_value_create_int64(123);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(value));
    lbug_value_destroy(value);
    value = lbug_value_create_null();
    ASSERT_TRUE(lbug_value_is_null(value));
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, SetNull) {
    lbug_value* value = lbug_value_create_int64(123);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(value));
    lbug_value_set_null(value, true);
    ASSERT_TRUE(lbug_value_is_null(value));
    lbug_value_set_null(value, false);
    ASSERT_FALSE(lbug_value_is_null(value));
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateDefault) {
    lbug_logical_type type;
    lbug_data_type_create(lbug_data_type_id::LBUG_INT64, nullptr, 0, &type);
    lbug_value* value = lbug_value_create_default(&type);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    lbug_data_type_destroy(&type);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_FALSE(lbug_value_is_null(value));
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::INT64);
    ASSERT_EQ(cppValue->getValue<int64_t>(), 0);
    lbug_value_destroy(value);

    lbug_data_type_create(lbug_data_type_id::LBUG_STRING, nullptr, 0, &type);
    value = lbug_value_create_default(&type);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    lbug_data_type_destroy(&type);
    cppValue = static_cast<Value*>(value->_value);
    ASSERT_FALSE(lbug_value_is_null(value));
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::STRING);
    ASSERT_EQ(cppValue->getValue<std::string>(), "");
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateBool) {
    lbug_value* value = lbug_value_create_bool(true);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::BOOL);
    ASSERT_EQ(cppValue->getValue<bool>(), true);
    lbug_value_destroy(value);

    value = lbug_value_create_bool(false);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::BOOL);
    ASSERT_EQ(cppValue->getValue<bool>(), false);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateInt8) {
    lbug_value* value = lbug_value_create_int8(12);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::INT8);
    ASSERT_EQ(cppValue->getValue<int8_t>(), 12);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateInt16) {
    lbug_value* value = lbug_value_create_int16(123);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::INT16);
    ASSERT_EQ(cppValue->getValue<int16_t>(), 123);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateInt32) {
    lbug_value* value = lbug_value_create_int32(123);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::INT32);
    ASSERT_EQ(cppValue->getValue<int32_t>(), 123);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateInt64) {
    lbug_value* value = lbug_value_create_int64(123);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::INT64);
    ASSERT_EQ(cppValue->getValue<int64_t>(), 123);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateUInt8) {
    lbug_value* value = lbug_value_create_uint8(12);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::UINT8);
    ASSERT_EQ(cppValue->getValue<uint8_t>(), 12);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateUInt16) {
    lbug_value* value = lbug_value_create_uint16(123);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::UINT16);
    ASSERT_EQ(cppValue->getValue<uint16_t>(), 123);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateUInt32) {
    lbug_value* value = lbug_value_create_uint32(123);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::UINT32);
    ASSERT_EQ(cppValue->getValue<uint32_t>(), 123);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateUInt64) {
    lbug_value* value = lbug_value_create_uint64(123);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::UINT64);
    ASSERT_EQ(cppValue->getValue<uint64_t>(), 123);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateINT128) {
    lbug_value* value = lbug_value_create_int128(lbug_int128_t{211111111, 100000000});
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::INT128);
    auto cppTimeStamp = cppValue->getValue<int128_t>();
    ASSERT_EQ(cppTimeStamp.high, 100000000);
    ASSERT_EQ(cppTimeStamp.low, 211111111);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateFloat) {
    lbug_value* value = lbug_value_create_float(123.456);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::FLOAT);
    ASSERT_FLOAT_EQ(cppValue->getValue<float>(), 123.456);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateDouble) {
    lbug_value* value = lbug_value_create_double(123.456);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::DOUBLE);
    ASSERT_DOUBLE_EQ(cppValue->getValue<double>(), 123.456);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateInternalID) {
    auto internalID = lbug_internal_id_t{1, 123};
    lbug_value* value = lbug_value_create_internal_id(internalID);
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::INTERNAL_ID);
    auto internalIDCpp = cppValue->getValue<internalID_t>();
    ASSERT_EQ(internalIDCpp.tableID, 1);
    ASSERT_EQ(internalIDCpp.offset, 123);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateDate) {
    lbug_value* value = lbug_value_create_date(lbug_date_t{123});
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::DATE);
    auto cppDate = cppValue->getValue<date_t>();
    ASSERT_EQ(cppDate.days, 123);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateTimeStamp) {
    lbug_value* value = lbug_value_create_timestamp(lbug_timestamp_t{123});
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::TIMESTAMP);
    auto cppTimeStamp = cppValue->getValue<timestamp_t>();
    ASSERT_EQ(cppTimeStamp.value, 123);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateTimeStampNonStandard) {
    lbug_value* value_ns = lbug_value_create_timestamp_ns(lbug_timestamp_ns_t{12345});
    lbug_value* value_ms = lbug_value_create_timestamp_ms(lbug_timestamp_ms_t{123456});
    lbug_value* value_sec = lbug_value_create_timestamp_sec(lbug_timestamp_sec_t{1234567});
    lbug_value* value_tz = lbug_value_create_timestamp_tz(lbug_timestamp_tz_t{12345678});

    ASSERT_FALSE(value_ns->_is_owned_by_cpp);
    ASSERT_FALSE(value_ms->_is_owned_by_cpp);
    ASSERT_FALSE(value_sec->_is_owned_by_cpp);
    ASSERT_FALSE(value_tz->_is_owned_by_cpp);
    auto cppValue_ns = static_cast<Value*>(value_ns->_value);
    auto cppValue_ms = static_cast<Value*>(value_ms->_value);
    auto cppValue_sec = static_cast<Value*>(value_sec->_value);
    auto cppValue_tz = static_cast<Value*>(value_tz->_value);
    ASSERT_EQ(cppValue_ns->getDataType().getLogicalTypeID(), LogicalTypeID::TIMESTAMP_NS);
    ASSERT_EQ(cppValue_ms->getDataType().getLogicalTypeID(), LogicalTypeID::TIMESTAMP_MS);
    ASSERT_EQ(cppValue_sec->getDataType().getLogicalTypeID(), LogicalTypeID::TIMESTAMP_SEC);
    ASSERT_EQ(cppValue_tz->getDataType().getLogicalTypeID(), LogicalTypeID::TIMESTAMP_TZ);

    auto cppTimeStamp_ns = cppValue_ns->getValue<timestamp_ns_t>();
    auto cppTimeStamp_ms = cppValue_ms->getValue<timestamp_ms_t>();
    auto cppTimeStamp_sec = cppValue_sec->getValue<timestamp_sec_t>();
    auto cppTimeStamp_tz = cppValue_tz->getValue<timestamp_tz_t>();
    ASSERT_EQ(cppTimeStamp_ns.value, 12345);
    ASSERT_EQ(cppTimeStamp_ms.value, 123456);
    ASSERT_EQ(cppTimeStamp_sec.value, 1234567);
    ASSERT_EQ(cppTimeStamp_tz.value, 12345678);
    lbug_value_destroy(value_ns);
    lbug_value_destroy(value_ms);
    lbug_value_destroy(value_sec);
    lbug_value_destroy(value_tz);
}

TEST(CApiValueTestEmptyDB, CreateInterval) {
    lbug_value* value = lbug_value_create_interval(lbug_interval_t{12, 3, 300});
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::INTERVAL);
    auto cppTimeStamp = cppValue->getValue<interval_t>();
    ASSERT_EQ(cppTimeStamp.months, 12);
    ASSERT_EQ(cppTimeStamp.days, 3);
    ASSERT_EQ(cppTimeStamp.micros, 300);
    lbug_value_destroy(value);
}

TEST(CApiValueTestEmptyDB, CreateString) {
    lbug_value* value = lbug_value_create_string((char*)"abcdefg");
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::STRING);
    ASSERT_EQ(cppValue->getValue<std::string>(), "abcdefg");
    lbug_value_destroy(value);
}

TEST_F(CApiValueTest, CreateList) {
    auto connection = getConnection();
    lbug_value* value1 = lbug_value_create_int64(123);
    lbug_value* value2 = lbug_value_create_int64(456);
    lbug_value* value3 = lbug_value_create_int64(789);
    lbug_value* value4 = lbug_value_create_int64(101112);
    lbug_value* value5 = lbug_value_create_int64(131415);
    lbug_value* elements[] = {value1, value2, value3, value4, value5};
    lbug_value* value = nullptr;
    lbug_state state = lbug_value_create_list(5, elements, &value);
    ASSERT_EQ(state, LbugSuccess);
    // Destroy the original values, the list should still be valid
    for (int i = 0; i < 5; ++i) {
        lbug_value_destroy(elements[i]);
    }
    ASSERT_FALSE(value->_is_owned_by_cpp);
    lbug_prepared_statement stmt;
    state = lbug_connection_prepare(connection, (char*)"RETURN $1", &stmt);
    ASSERT_EQ(state, LbugSuccess);
    state = lbug_prepared_statement_bind_value(&stmt, "1", value);
    ASSERT_EQ(state, LbugSuccess);
    lbug_query_result result;
    state = lbug_connection_execute(connection, &stmt, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    lbug_flat_tuple flatTuple;
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value outValue;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &outValue), LbugSuccess);
    ASSERT_TRUE(outValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&outValue));
    uint64_t size;
    ASSERT_EQ(lbug_value_get_list_size(&outValue, &size), LbugSuccess);
    ASSERT_EQ(size, 5);
    lbug_value listElement;
    ASSERT_EQ(lbug_value_get_list_element(&outValue, 0, &listElement), LbugSuccess);
    ASSERT_TRUE(listElement._is_owned_by_cpp);
    int64_t int64Result;
    ASSERT_EQ(lbug_value_get_int64(&listElement, &int64Result), LbugSuccess);
    ASSERT_EQ(int64Result, 123);
    lbug_value_destroy(&listElement);
    ASSERT_EQ(lbug_value_get_list_element(&outValue, 1, &listElement), LbugSuccess);
    ASSERT_TRUE(listElement._is_owned_by_cpp);
    ASSERT_EQ(lbug_value_get_int64(&listElement, &int64Result), LbugSuccess);
    ASSERT_EQ(int64Result, 456);
    lbug_value_destroy(&listElement);
    ASSERT_EQ(lbug_value_get_list_element(&outValue, 2, &listElement), LbugSuccess);
    ASSERT_TRUE(listElement._is_owned_by_cpp);
    ASSERT_EQ(lbug_value_get_int64(&listElement, &int64Result), LbugSuccess);
    ASSERT_EQ(int64Result, 789);
    lbug_value_destroy(&listElement);
    ASSERT_EQ(lbug_value_get_list_element(&outValue, 3, &listElement), LbugSuccess);
    ASSERT_TRUE(listElement._is_owned_by_cpp);
    ASSERT_EQ(lbug_value_get_int64(&listElement, &int64Result), LbugSuccess);
    ASSERT_EQ(int64Result, 101112);
    lbug_value_destroy(&listElement);
    ASSERT_EQ(lbug_value_get_list_element(&outValue, 4, &listElement), LbugSuccess);
    ASSERT_TRUE(listElement._is_owned_by_cpp);
    ASSERT_EQ(lbug_value_get_int64(&listElement, &int64Result), LbugSuccess);
    ASSERT_EQ(int64Result, 131415);
    lbug_value_destroy(&listElement);
    lbug_value_destroy(&outValue);
    lbug_value_destroy(value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&stmt);
}

TEST(CApiValueTestEmptyDB, CreateListDifferentTypes) {
    lbug_value* value1 = lbug_value_create_int64(123);
    lbug_value* value2 = lbug_value_create_string((char*)"abcdefg");
    lbug_value* elements[] = {value1, value2};
    lbug_value* value = nullptr;
    lbug_state state = lbug_value_create_list(2, elements, &value);
    ASSERT_EQ(state, LbugError);
    lbug_value_destroy(value1);
    lbug_value_destroy(value2);
}

TEST(CApiValueTestEmptyDB, CreateListEmpty) {
    lbug_value* elements[] = {nullptr}; // Must be non-empty
    lbug_value* value = nullptr;
    lbug_state state = lbug_value_create_list(0, elements, &value);
    ASSERT_EQ(state, LbugError);
}

TEST_F(CApiValueTest, CreateListNested) {
    auto connection = getConnection();
    lbug_value* value1 = lbug_value_create_int64(123);
    lbug_value* value2 = lbug_value_create_int64(456);
    lbug_value* value3 = lbug_value_create_int64(789);
    lbug_value* value4 = lbug_value_create_int64(101112);
    lbug_value* value5 = lbug_value_create_int64(131415);
    lbug_value* elements1[] = {value1, value2, value3};
    lbug_value* elements2[] = {value4, value5};
    lbug_value* list1 = nullptr;
    lbug_value* list2 = nullptr;
    lbug_value_create_list(3, elements1, &list1);
    ASSERT_FALSE(list1->_is_owned_by_cpp);
    lbug_value_create_list(2, elements2, &list2);
    ASSERT_FALSE(list2->_is_owned_by_cpp);
    lbug_value* elements[] = {list1, list2};
    lbug_value* nestedList = nullptr;
    lbug_state state = lbug_value_create_list(2, elements, &nestedList);
    ASSERT_EQ(state, LbugSuccess);
    // Destroy the original values, the list should still be valid
    for (int i = 0; i < 3; ++i) {
        lbug_value_destroy(elements1[i]);
    }
    for (int i = 0; i < 2; ++i) {
        lbug_value_destroy(elements2[i]);
    }
    lbug_value_destroy(list1);
    lbug_value_destroy(list2);
    ASSERT_FALSE(nestedList->_is_owned_by_cpp);
    lbug_prepared_statement stmt;
    state = lbug_connection_prepare(connection, (char*)"RETURN $1", &stmt);
    ASSERT_EQ(state, LbugSuccess);
    state = lbug_prepared_statement_bind_value(&stmt, "1", nestedList);
    ASSERT_EQ(state, LbugSuccess);
    lbug_query_result result;
    state = lbug_connection_execute(connection, &stmt, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    lbug_flat_tuple flatTuple;
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value outValue;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &outValue), LbugSuccess);
    ASSERT_TRUE(outValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&outValue));
    uint64_t size;
    ASSERT_EQ(lbug_value_get_list_size(&outValue, &size), LbugSuccess);
    ASSERT_EQ(size, 2);
    lbug_value listElement;
    ASSERT_EQ(lbug_value_get_list_element(&outValue, 0, &listElement), LbugSuccess);
    ASSERT_TRUE(listElement._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&listElement));
    ASSERT_EQ(lbug_value_get_list_size(&listElement, &size), LbugSuccess);
    ASSERT_EQ(size, 3);
    lbug_value innerListElement;
    ASSERT_EQ(lbug_value_get_list_element(&listElement, 0, &innerListElement), LbugSuccess);
    ASSERT_TRUE(innerListElement._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&innerListElement));
    int64_t int64Result;
    ASSERT_EQ(lbug_value_get_int64(&innerListElement, &int64Result), LbugSuccess);
    ASSERT_EQ(int64Result, 123);
    lbug_value_destroy(&innerListElement);
    ASSERT_EQ(lbug_value_get_list_element(&listElement, 1, &innerListElement), LbugSuccess);
    ASSERT_TRUE(innerListElement._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&innerListElement));
    ASSERT_EQ(lbug_value_get_int64(&innerListElement, &int64Result), LbugSuccess);
    ASSERT_EQ(int64Result, 456);
    lbug_value_destroy(&innerListElement);
    ASSERT_EQ(lbug_value_get_list_element(&listElement, 2, &innerListElement), LbugSuccess);
    ASSERT_TRUE(innerListElement._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&innerListElement));
    ASSERT_EQ(lbug_value_get_int64(&innerListElement, &int64Result), LbugSuccess);
    ASSERT_EQ(int64Result, 789);
    lbug_value_destroy(&innerListElement);
    lbug_value_destroy(&listElement);
    ASSERT_EQ(lbug_value_get_list_element(&outValue, 1, &listElement), LbugSuccess);
    ASSERT_TRUE(listElement._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&listElement));
    ASSERT_EQ(lbug_value_get_list_size(&listElement, &size), LbugSuccess);
    ASSERT_EQ(size, 2);
    lbug_value_destroy(&listElement);
    lbug_value_destroy(&outValue);
    lbug_value_destroy(nestedList);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&stmt);
}

TEST_F(CApiValueTest, CreateStruct) {
    auto connection = getConnection();
    lbug_value* value1 = lbug_value_create_int16(32);
    lbug_value* value2 = lbug_value_create_string((char*)"Wong");
    lbug_value* value3 = lbug_value_create_string((char*)"Kelley");
    lbug_value* value4 = lbug_value_create_int64(123456);
    lbug_value* value5 = lbug_value_create_string((char*)"CEO");
    lbug_value* value6 = lbug_value_create_bool(true);
    lbug_value* employmentElements[] = {value5, value6};
    const char* employmentFieldNames[] = {(char*)"title", (char*)"is_current"};
    lbug_value* employment = nullptr;
    lbug_state state =
        lbug_value_create_struct(2, employmentFieldNames, employmentElements, &employment);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_FALSE(employment->_is_owned_by_cpp);
    lbug_value_destroy(value5);
    lbug_value_destroy(value6);
    lbug_value* personElements[] = {value1, value2, value3, value4, employment};
    const char* personFieldNames[] = {(char*)"age", (char*)"first_name", (char*)"last_name",
        (char*)"id", (char*)"employment"};
    lbug_value* person = nullptr;
    state = lbug_value_create_struct(5, personFieldNames, personElements, &person);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value_destroy(value1);
    lbug_value_destroy(value2);
    lbug_value_destroy(value3);
    lbug_value_destroy(value4);
    lbug_value_destroy(employment);
    ASSERT_FALSE(person->_is_owned_by_cpp);
    lbug_prepared_statement stmt;
    state = lbug_connection_prepare(connection, (char*)"RETURN $1", &stmt);
    ASSERT_EQ(state, LbugSuccess);
    state = lbug_prepared_statement_bind_value(&stmt, "1", person);
    ASSERT_EQ(state, LbugSuccess);
    lbug_query_result result;
    state = lbug_connection_execute(connection, &stmt, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    lbug_flat_tuple flatTuple;
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value outValue;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &outValue), LbugSuccess);
    ASSERT_TRUE(outValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&outValue));
    uint64_t size;
    state = lbug_value_get_struct_num_fields(&outValue, &size);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_EQ(size, 5);
    char* structFieldName;
    lbug_value structFieldValue;
    state = lbug_value_get_struct_field_name(&outValue, 0, &structFieldName);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_STREQ(structFieldName, "age");
    state = lbug_value_get_struct_field_value(&outValue, 0, &structFieldValue);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(structFieldValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&structFieldValue));
    int16_t int16Result;
    state = lbug_value_get_int16(&structFieldValue, &int16Result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_EQ(int16Result, 32);
    lbug_value_destroy(&structFieldValue);
    lbug_destroy_string(structFieldName);
    state = lbug_value_get_struct_field_name(&outValue, 1, &structFieldName);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_STREQ(structFieldName, "first_name");
    state = lbug_value_get_struct_field_value(&outValue, 1, &structFieldValue);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(structFieldValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&structFieldValue));
    char* stringResult;
    state = lbug_value_get_string(&structFieldValue, &stringResult);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_STREQ(stringResult, "Wong");
    lbug_value_destroy(&structFieldValue);
    lbug_destroy_string(structFieldName);
    lbug_destroy_string(stringResult);
    state = lbug_value_get_struct_field_name(&outValue, 2, &structFieldName);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_STREQ(structFieldName, "last_name");
    state = lbug_value_get_struct_field_value(&outValue, 2, &structFieldValue);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(structFieldValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&structFieldValue));
    state = lbug_value_get_string(&structFieldValue, &stringResult);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_STREQ(stringResult, "Kelley");
    lbug_value_destroy(&structFieldValue);
    lbug_destroy_string(structFieldName);
    lbug_destroy_string(stringResult);
    state = lbug_value_get_struct_field_name(&outValue, 3, &structFieldName);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_STREQ(structFieldName, "id");
    state = lbug_value_get_struct_field_value(&outValue, 3, &structFieldValue);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(structFieldValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&structFieldValue));
    int64_t int64Result;
    state = lbug_value_get_int64(&structFieldValue, &int64Result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_EQ(int64Result, 123456);
    lbug_value_destroy(&structFieldValue);
    lbug_destroy_string(structFieldName);
    state = lbug_value_get_struct_field_name(&outValue, 4, &structFieldName);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_STREQ(structFieldName, "employment");
    state = lbug_value_get_struct_field_value(&outValue, 4, &structFieldValue);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(structFieldValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&structFieldValue));
    state = lbug_value_get_struct_num_fields(&structFieldValue, &size);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_EQ(size, 2);
    char* employmentFieldName;
    lbug_value employmentFieldValue;
    state = lbug_value_get_struct_field_name(&structFieldValue, 0, &employmentFieldName);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_STREQ(employmentFieldName, "title");
    state = lbug_value_get_struct_field_value(&structFieldValue, 0, &employmentFieldValue);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(employmentFieldValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&employmentFieldValue));
    state = lbug_value_get_string(&employmentFieldValue, &stringResult);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_STREQ(stringResult, "CEO");
    lbug_value_destroy(&employmentFieldValue);
    lbug_destroy_string(employmentFieldName);
    lbug_destroy_string(stringResult);
    state = lbug_value_get_struct_field_name(&structFieldValue, 1, &employmentFieldName);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_STREQ(employmentFieldName, "is_current");
    state = lbug_value_get_struct_field_value(&structFieldValue, 1, &employmentFieldValue);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(employmentFieldValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&employmentFieldValue));
    bool boolResult;
    state = lbug_value_get_bool(&employmentFieldValue, &boolResult);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_EQ(boolResult, true);
    lbug_value_destroy(&employmentFieldValue);
    lbug_destroy_string(employmentFieldName);
    lbug_value_destroy(&structFieldValue);
    lbug_destroy_string(structFieldName);
    lbug_value_destroy(&outValue);
    lbug_value_destroy(person);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&stmt);
}

TEST(CApiValueTestEmptyDB, CreateStructEmpty) {
    const char* fieldNames[] = {(char*)"name"}; // Must be non-empty
    lbug_value* values[] = {nullptr};           // Must be non-empty
    lbug_value* value = nullptr;
    lbug_state state = lbug_value_create_struct(0, fieldNames, values, &value);
    ASSERT_EQ(state, LbugError);
}

TEST_F(CApiValueTest, CreateMap) {
    auto connection = getConnection();
    lbug_value* key1 = lbug_value_create_int64(1);
    lbug_value* value1 = lbug_value_create_string((char*)"one");
    lbug_value* key2 = lbug_value_create_int64(2);
    lbug_value* value2 = lbug_value_create_string((char*)"two");
    lbug_value* key3 = lbug_value_create_int64(3);
    lbug_value* value3 = lbug_value_create_string((char*)"three");
    lbug_value* keys[] = {key1, key2, key3};
    lbug_value* values[] = {value1, value2, value3};
    lbug_value* map = nullptr;
    lbug_state state = lbug_value_create_map(3, keys, values, &map);
    ASSERT_EQ(state, LbugSuccess);
    // Destroy the original values, the map should still be valid
    for (int i = 0; i < 3; ++i) {
        lbug_value_destroy(keys[i]);
        lbug_value_destroy(values[i]);
    }
    ASSERT_FALSE(map->_is_owned_by_cpp);
    lbug_prepared_statement stmt;
    state = lbug_connection_prepare(connection, (char*)"RETURN $1", &stmt);
    ASSERT_EQ(state, LbugSuccess);
    state = lbug_prepared_statement_bind_value(&stmt, "1", map);
    ASSERT_EQ(state, LbugSuccess);
    lbug_query_result result;
    state = lbug_connection_execute(connection, &stmt, &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    lbug_flat_tuple flatTuple;
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value outValue;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &outValue), LbugSuccess);
    ASSERT_TRUE(outValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&outValue));
    uint64_t size;
    ASSERT_EQ(lbug_value_get_map_size(&outValue, &size), LbugSuccess);
    ASSERT_EQ(size, 3);
    lbug_value mapValue;
    ASSERT_EQ(lbug_value_get_map_value(&outValue, 0, &mapValue), LbugSuccess);
    ASSERT_TRUE(mapValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&mapValue));
    char* stringResult;
    ASSERT_EQ(lbug_value_get_string(&mapValue, &stringResult), LbugSuccess);
    ASSERT_STREQ(stringResult, "one");
    lbug_value_destroy(&mapValue);
    lbug_destroy_string(stringResult);
    ASSERT_EQ(lbug_value_get_map_value(&outValue, 1, &mapValue), LbugSuccess);
    ASSERT_TRUE(mapValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&mapValue));
    ASSERT_EQ(lbug_value_get_string(&mapValue, &stringResult), LbugSuccess);
    ASSERT_STREQ(stringResult, "two");
    lbug_value_destroy(&mapValue);
    lbug_destroy_string(stringResult);
    ASSERT_EQ(lbug_value_get_map_value(&outValue, 2, &mapValue), LbugSuccess);
    ASSERT_TRUE(mapValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&mapValue));
    ASSERT_EQ(lbug_value_get_string(&mapValue, &stringResult), LbugSuccess);
    ASSERT_STREQ(stringResult, "three");
    lbug_value_destroy(&mapValue);
    lbug_destroy_string(stringResult);
    ASSERT_EQ(lbug_value_get_map_key(&outValue, 0, &mapValue), LbugSuccess);
    ASSERT_TRUE(mapValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&mapValue));
    int64_t int64Result;
    ASSERT_EQ(lbug_value_get_int64(&mapValue, &int64Result), LbugSuccess);
    ASSERT_EQ(int64Result, 1);
    lbug_value_destroy(&mapValue);
    ASSERT_EQ(lbug_value_get_map_key(&outValue, 1, &mapValue), LbugSuccess);
    ASSERT_TRUE(mapValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&mapValue));
    ASSERT_EQ(lbug_value_get_int64(&mapValue, &int64Result), LbugSuccess);
    ASSERT_EQ(int64Result, 2);
    lbug_value_destroy(&mapValue);
    ASSERT_EQ(lbug_value_get_map_key(&outValue, 2, &mapValue), LbugSuccess);
    ASSERT_TRUE(mapValue._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&mapValue));
    ASSERT_EQ(lbug_value_get_int64(&mapValue, &int64Result), LbugSuccess);
    ASSERT_EQ(int64Result, 3);
    lbug_value_destroy(&mapValue);
    lbug_value_destroy(&outValue);
    lbug_value_destroy(map);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);
    lbug_prepared_statement_destroy(&stmt);
}

TEST(CApiValueTestEmptyDB, CreateMapEmpty) {
    lbug_value* keys[] = {nullptr};   // Must be non-empty
    lbug_value* values[] = {nullptr}; // Must be non-empty
    lbug_value* map = nullptr;
    lbug_state state = lbug_value_create_map(0, keys, values, &map);
    ASSERT_EQ(state, LbugError);
}

TEST(CApiValueTestEmptyDB, Clone) {
    lbug_value* value = lbug_value_create_string((char*)"abcdefg");
    ASSERT_FALSE(value->_is_owned_by_cpp);
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::STRING);
    ASSERT_EQ(cppValue->getValue<std::string>(), "abcdefg");

    lbug_value* clone = lbug_value_clone(value);
    lbug_value_destroy(value);

    ASSERT_FALSE(clone->_is_owned_by_cpp);
    auto cppClone = static_cast<Value*>(clone->_value);
    ASSERT_EQ(cppClone->getDataType().getLogicalTypeID(), LogicalTypeID::STRING);
    ASSERT_EQ(cppClone->getValue<std::string>(), "abcdefg");
    lbug_value_destroy(clone);
}

TEST(CApiValueTestEmptyDB, Copy) {
    lbug_value* value = lbug_value_create_string((char*)"abc");

    lbug_value* value2 = lbug_value_create_string((char*)"abcdefg");
    lbug_value_copy(value, value2);
    lbug_value_destroy(value2);

    ASSERT_FALSE(lbug_value_is_null(value));
    auto cppValue = static_cast<Value*>(value->_value);
    ASSERT_EQ(cppValue->getDataType().getLogicalTypeID(), LogicalTypeID::STRING);
    ASSERT_EQ(cppValue->getValue<std::string>(), "abcdefg");
    lbug_value_destroy(value);
}

TEST_F(CApiValueTest, GetListSize) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) RETURN a.workedHours ORDER BY a.ID", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    uint64_t size;
    ASSERT_EQ(lbug_value_get_list_size(&value, &size), LbugSuccess);
    ASSERT_EQ(size, 2);

    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_list_size(badValue, &size), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetListElement) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) RETURN a.workedHours ORDER BY a.ID", &result);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    uint64_t size;
    ASSERT_EQ(lbug_value_get_list_size(&value, &size), LbugSuccess);
    ASSERT_EQ(size, 2);

    lbug_value listElement;
    ASSERT_EQ(lbug_value_get_list_element(&value, 0, &listElement), LbugSuccess);
    ASSERT_TRUE(listElement._is_owned_by_cpp);
    int64_t int64Result;
    ASSERT_EQ(lbug_value_get_int64(&listElement, &int64Result), LbugSuccess);
    ASSERT_EQ(int64Result, 10);

    ASSERT_EQ(lbug_value_get_list_element(&value, 1, &listElement), LbugSuccess);
    ASSERT_TRUE(listElement._is_owned_by_cpp);
    ASSERT_EQ(lbug_value_get_int64(&listElement, &int64Result), LbugSuccess);
    ASSERT_EQ(int64Result, 5);
    lbug_value_destroy(&listElement);

    ASSERT_EQ(lbug_value_get_list_element(&value, 222, &listElement), LbugError);

    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);
}

TEST_F(CApiValueTest, GetStructNumFields) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (m:movies) WHERE m.name=\"Roma\" RETURN m.description", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    lbug_flat_tuple_get_value(&flatTuple, 0, &value);
    uint64_t numFields;
    ASSERT_EQ(lbug_value_get_struct_num_fields(&value, &numFields), LbugSuccess);
    ASSERT_EQ(numFields, 14);

    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_struct_num_fields(badValue, &numFields), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetStructFieldName) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (m:movies) WHERE m.name=\"Roma\" RETURN m.description", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    char* fieldName;
    ASSERT_EQ(lbug_value_get_struct_field_name(&value, 0, &fieldName), LbugSuccess);
    ASSERT_STREQ(fieldName, "rating");
    lbug_destroy_string(fieldName);

    ASSERT_EQ(lbug_value_get_struct_field_name(&value, 1, &fieldName), LbugSuccess);
    ASSERT_STREQ(fieldName, "stars");
    lbug_destroy_string(fieldName);

    ASSERT_EQ(lbug_value_get_struct_field_name(&value, 2, &fieldName), LbugSuccess);
    ASSERT_STREQ(fieldName, "views");
    lbug_destroy_string(fieldName);

    ASSERT_EQ(lbug_value_get_struct_field_name(&value, 3, &fieldName), LbugSuccess);
    ASSERT_STREQ(fieldName, "release");
    lbug_destroy_string(fieldName);

    ASSERT_EQ(lbug_value_get_struct_field_name(&value, 4, &fieldName), LbugSuccess);
    ASSERT_STREQ(fieldName, "release_ns");
    lbug_destroy_string(fieldName);

    ASSERT_EQ(lbug_value_get_struct_field_name(&value, 5, &fieldName), LbugSuccess);
    ASSERT_STREQ(fieldName, "release_ms");
    lbug_destroy_string(fieldName);

    ASSERT_EQ(lbug_value_get_struct_field_name(&value, 6, &fieldName), LbugSuccess);
    ASSERT_STREQ(fieldName, "release_sec");
    lbug_destroy_string(fieldName);

    ASSERT_EQ(lbug_value_get_struct_field_name(&value, 7, &fieldName), LbugSuccess);
    ASSERT_STREQ(fieldName, "release_tz");
    lbug_destroy_string(fieldName);

    ASSERT_EQ(lbug_value_get_struct_field_name(&value, 8, &fieldName), LbugSuccess);
    ASSERT_STREQ(fieldName, "film");
    lbug_destroy_string(fieldName);

    ASSERT_EQ(lbug_value_get_struct_field_name(&value, 222, &fieldName), LbugError);

    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);
}

TEST_F(CApiValueTest, GetStructFieldValue) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (m:movies) WHERE m.name=\"Roma\" RETURN m.description", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);

    lbug_value fieldValue;
    ASSERT_EQ(lbug_value_get_struct_field_value(&value, 0, &fieldValue), LbugSuccess);
    lbug_logical_type fieldType;
    lbug_value_get_data_type(&fieldValue, &fieldType);
    ASSERT_EQ(lbug_data_type_get_id(&fieldType), LBUG_DOUBLE);
    double doubleValue;
    ASSERT_EQ(lbug_value_get_double(&fieldValue, &doubleValue), LbugSuccess);
    ASSERT_DOUBLE_EQ(doubleValue, 1223);
    lbug_data_type_destroy(&fieldType);

    ASSERT_EQ(lbug_value_get_struct_field_value(&value, 1, &fieldValue), LbugSuccess);
    lbug_value_get_data_type(&fieldValue, &fieldType);
    lbug_data_type_destroy(&fieldType);

    ASSERT_EQ(lbug_value_get_struct_field_value(&value, 2, &fieldValue), LbugSuccess);
    lbug_value_get_data_type(&fieldValue, &fieldType);
    ASSERT_EQ(lbug_data_type_get_id(&fieldType), LBUG_INT64);
    int64_t int64Value;
    ASSERT_EQ(lbug_value_get_int64(&fieldValue, &int64Value), LbugSuccess);
    lbug_data_type_destroy(&fieldType);

    ASSERT_EQ(lbug_value_get_struct_field_value(&value, 3, &fieldValue), LbugSuccess);
    lbug_value_get_data_type(&fieldValue, &fieldType);
    ASSERT_EQ(lbug_data_type_get_id(&fieldType), LBUG_TIMESTAMP);
    lbug_timestamp_t timestamp;
    ASSERT_EQ(lbug_value_get_timestamp(&fieldValue, &timestamp), LbugSuccess);
    ASSERT_EQ(timestamp.value, 1297442662000000);
    lbug_data_type_destroy(&fieldType);

    ASSERT_EQ(lbug_value_get_struct_field_value(&value, 4, &fieldValue), LbugSuccess);
    lbug_value_get_data_type(&fieldValue, &fieldType);
    ASSERT_EQ(lbug_data_type_get_id(&fieldType), LBUG_TIMESTAMP_NS);
    lbug_timestamp_ns_t timestamp_ns;
    ASSERT_EQ(lbug_value_get_timestamp_ns(&fieldValue, &timestamp_ns), LbugSuccess);
    ASSERT_EQ(timestamp_ns.value, 1297442662123456000);
    lbug_data_type_destroy(&fieldType);

    ASSERT_EQ(lbug_value_get_struct_field_value(&value, 5, &fieldValue), LbugSuccess);
    lbug_value_get_data_type(&fieldValue, &fieldType);
    ASSERT_EQ(lbug_data_type_get_id(&fieldType), LBUG_TIMESTAMP_MS);
    lbug_timestamp_ms_t timestamp_ms;
    ASSERT_EQ(lbug_value_get_timestamp_ms(&fieldValue, &timestamp_ms), LbugSuccess);
    ASSERT_EQ(timestamp_ms.value, 1297442662123);
    lbug_data_type_destroy(&fieldType);

    ASSERT_EQ(lbug_value_get_struct_field_value(&value, 6, &fieldValue), LbugSuccess);
    lbug_value_get_data_type(&fieldValue, &fieldType);
    ASSERT_EQ(lbug_data_type_get_id(&fieldType), LBUG_TIMESTAMP_SEC);
    lbug_timestamp_sec_t timestamp_sec;
    ASSERT_EQ(lbug_value_get_timestamp_sec(&fieldValue, &timestamp_sec), LbugSuccess);
    ASSERT_EQ(timestamp_sec.value, 1297442662);
    lbug_data_type_destroy(&fieldType);

    ASSERT_EQ(lbug_value_get_struct_field_value(&value, 7, &fieldValue), LbugSuccess);
    lbug_value_get_data_type(&fieldValue, &fieldType);
    ASSERT_EQ(lbug_data_type_get_id(&fieldType), LBUG_TIMESTAMP_TZ);
    lbug_timestamp_tz_t timestamp_tz;
    ASSERT_EQ(lbug_value_get_timestamp_tz(&fieldValue, &timestamp_tz), LbugSuccess);
    ASSERT_EQ(timestamp_tz.value, 1297442662123456);
    lbug_data_type_destroy(&fieldType);

    ASSERT_EQ(lbug_value_get_struct_field_value(&value, 8, &fieldValue), LbugSuccess);
    lbug_value_get_data_type(&fieldValue, &fieldType);
    ASSERT_EQ(lbug_data_type_get_id(&fieldType), LBUG_DATE);
    lbug_date_t date;
    ASSERT_EQ(lbug_value_get_date(&fieldValue, &date), LbugSuccess);
    ASSERT_EQ(date.days, 15758);
    lbug_data_type_destroy(&fieldType);
    lbug_value_destroy(&fieldValue);

    ASSERT_EQ(lbug_value_get_struct_field_value(&value, 222, &fieldValue), LbugError);

    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);
}

TEST_F(CApiValueTest, getMapNumFields) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (m:movies) WHERE m.length = 2544 RETURN m.audience", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_FALSE(lbug_query_result_has_next(&result));
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);

    uint64_t mapFields;
    ASSERT_EQ(lbug_value_get_map_size(&value, &mapFields), LbugSuccess);
    ASSERT_EQ(mapFields, 1);

    lbug_query_result_destroy(&result);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
}

TEST_F(CApiValueTest, getMapKey) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (m:movies) WHERE m.length = 2544 RETURN m.audience", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_FALSE(lbug_query_result_has_next(&result));
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);

    lbug_value key;
    ASSERT_EQ(lbug_value_get_map_key(&value, 0, &key), LbugSuccess);
    lbug_logical_type keyType;
    lbug_value_get_data_type(&key, &keyType);
    ASSERT_EQ(lbug_data_type_get_id(&keyType), LBUG_STRING);
    char* mapName;
    ASSERT_EQ(lbug_value_get_string(&key, &mapName), LbugSuccess);
    ASSERT_STREQ(mapName, "audience1");
    lbug_destroy_string(mapName);
    lbug_data_type_destroy(&keyType);
    lbug_value_destroy(&key);

    ASSERT_EQ(lbug_value_get_map_key(&value, 1, &key), LbugError);
    lbug_query_result_destroy(&result);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
}

TEST_F(CApiValueTest, getMapValue) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (m:movies) WHERE m.length = 2544 RETURN m.audience", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_FALSE(lbug_query_result_has_next(&result));
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);

    lbug_value mapValue;
    ASSERT_EQ(lbug_value_get_map_value(&value, 0, &mapValue), LbugSuccess);
    lbug_logical_type mapType;
    lbug_value_get_data_type(&mapValue, &mapType);
    ASSERT_EQ(lbug_data_type_get_id(&mapType), LBUG_INT64);
    int64_t mapIntValue;
    ASSERT_EQ(lbug_value_get_int64(&mapValue, &mapIntValue), LbugSuccess);
    ASSERT_EQ(mapIntValue, 33);

    ASSERT_EQ(lbug_value_get_map_value(&value, 1, &mapValue), LbugError);

    lbug_data_type_destroy(&mapType);
    lbug_query_result_destroy(&result);
    lbug_value_destroy(&mapValue);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
}

TEST_F(CApiValueTest, getDecimalAsString) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"UNWIND [1] AS A UNWIND [5.7, 8.3, 8.7, 13.7] AS B WITH cast(CAST(A AS DECIMAL) "
               "* "
               "CAST(B AS DECIMAL) AS DECIMAL(18, 1)) AS PROD RETURN COLLECT(PROD) AS RES",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);

    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);

    lbug_logical_type dataType;
    lbug_value_get_data_type(&value, &dataType);
    ASSERT_EQ(lbug_data_type_get_id(&dataType), LBUG_LIST);
    uint64_t list_size;
    ASSERT_EQ(lbug_value_get_list_size(&value, &list_size), LbugSuccess);
    ASSERT_EQ(list_size, 4);
    lbug_data_type_destroy(&dataType);

    lbug_value decimal_entry;
    char* decimal_value;
    std::string decimal_string_value;
    ASSERT_EQ(lbug_value_get_list_element(&value, 0, &decimal_entry), LbugSuccess);
    lbug_value_get_data_type(&decimal_entry, &dataType);
    ASSERT_EQ(lbug_data_type_get_id(&dataType), LBUG_DECIMAL);
    ASSERT_EQ(lbug_value_get_decimal_as_string(&decimal_entry, &decimal_value), LbugSuccess);
    decimal_string_value = std::string(decimal_value);
    ASSERT_EQ(decimal_string_value, "5.7");
    lbug_destroy_string(decimal_value);
    lbug_data_type_destroy(&dataType);

    ASSERT_EQ(lbug_value_get_list_element(&value, 1, &decimal_entry), LbugSuccess);
    lbug_value_get_data_type(&decimal_entry, &dataType);
    ASSERT_EQ(lbug_data_type_get_id(&dataType), LBUG_DECIMAL);
    ASSERT_EQ(lbug_value_get_decimal_as_string(&decimal_entry, &decimal_value), LbugSuccess);
    decimal_string_value = std::string(decimal_value);
    ASSERT_EQ(decimal_string_value, "8.3");
    lbug_destroy_string(decimal_value);
    lbug_data_type_destroy(&dataType);

    ASSERT_EQ(lbug_value_get_list_element(&value, 2, &decimal_entry), LbugSuccess);
    lbug_value_get_data_type(&decimal_entry, &dataType);
    ASSERT_EQ(lbug_data_type_get_id(&dataType), LBUG_DECIMAL);
    ASSERT_EQ(lbug_value_get_decimal_as_string(&decimal_entry, &decimal_value), LbugSuccess);
    decimal_string_value = std::string(decimal_value);
    ASSERT_EQ(decimal_string_value, "8.7");
    lbug_destroy_string(decimal_value);
    lbug_data_type_destroy(&dataType);

    ASSERT_EQ(lbug_value_get_list_element(&value, 3, &decimal_entry), LbugSuccess);
    lbug_value_get_data_type(&decimal_entry, &dataType);
    ASSERT_EQ(lbug_data_type_get_id(&dataType), LBUG_DECIMAL);
    ASSERT_EQ(lbug_value_get_decimal_as_string(&decimal_entry, &decimal_value), LbugSuccess);
    decimal_string_value = std::string(decimal_value);
    ASSERT_EQ(decimal_string_value, "13.7");
    lbug_destroy_string(decimal_value);
    lbug_data_type_destroy(&dataType);

    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);
    lbug_value_destroy(&decimal_entry);
}

TEST_F(CApiValueTest, GetDataType) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) RETURN a.fName, a.isStudent, a.workedHours", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    lbug_logical_type dataType;
    lbug_value_get_data_type(&value, &dataType);
    ASSERT_EQ(lbug_data_type_get_id(&dataType), LBUG_STRING);
    lbug_data_type_destroy(&dataType);

    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 1, &value), LbugSuccess);
    lbug_value_get_data_type(&value, &dataType);
    ASSERT_EQ(lbug_data_type_get_id(&dataType), LBUG_BOOL);
    lbug_data_type_destroy(&dataType);

    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 2, &value), LbugSuccess);
    lbug_value_get_data_type(&value, &dataType);
    ASSERT_EQ(lbug_data_type_get_id(&dataType), LBUG_LIST);
    lbug_data_type_destroy(&dataType);
    lbug_value_destroy(&value);

    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);
}

TEST_F(CApiValueTest, GetBool) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) RETURN a.isStudent ORDER BY a.ID", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    bool boolValue;
    ASSERT_EQ(lbug_value_get_bool(&value, &boolValue), LbugSuccess);
    ASSERT_TRUE(boolValue);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_bool(badValue, &boolValue), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetInt8) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) -[r:studyAt]-> (b:organisation) RETURN r.level ORDER BY a.ID",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    int8_t int8Value;
    ASSERT_EQ(lbug_value_get_int8(&value, &int8Value), LbugSuccess);
    ASSERT_EQ(int8Value, 5);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_int8(badValue, &int8Value), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetInt16) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) -[r:studyAt]-> (b:organisation) RETURN r.length ORDER BY a.ID",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    int16_t int16Value;
    ASSERT_EQ(lbug_value_get_int16(&value, &int16Value), LbugSuccess);
    ASSERT_EQ(int16Value, 5);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_int16(badValue, &int16Value), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetInt32) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (m:movies) RETURN m.length ORDER BY m.name", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    int32_t int32Value;
    ASSERT_EQ(lbug_value_get_int32(&value, &int32Value), LbugSuccess);
    ASSERT_EQ(int32Value, 298);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_int32(badValue, &int32Value), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetInt64) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection, (char*)"MATCH (a:person) RETURN a.ID ORDER BY a.ID",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    int64_t int64Value;
    ASSERT_EQ(lbug_value_get_int64(&value, &int64Value), LbugSuccess);
    ASSERT_EQ(int64Value, 0);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_int64(badValue, &int64Value), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetUInt8) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) -[r:studyAt]-> (b:organisation) RETURN r.ulevel ORDER BY a.ID",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    uint8_t uint8Value;
    ASSERT_EQ(lbug_value_get_uint8(&value, &uint8Value), LbugSuccess);
    ASSERT_EQ(uint8Value, 250);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_uint8(badValue, &uint8Value), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetUInt16) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) -[r:studyAt]-> (b:organisation) RETURN r.ulength ORDER BY "
               "a.ID",
        &result);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    uint16_t uint16Value;
    ASSERT_EQ(lbug_value_get_uint16(&value, &uint16Value), LbugSuccess);
    ASSERT_EQ(uint16Value, 33768);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_uint16(badValue, &uint16Value), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetUInt32) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) -[r:studyAt]-> (b:organisation) "
               "RETURN r.temperature ORDER BY a.ID",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    uint32_t uint32Value;
    ASSERT_EQ(lbug_value_get_uint32(&value, &uint32Value), LbugSuccess);
    ASSERT_EQ(uint32Value, 32800);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_uint32(badValue, &uint32Value), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetUInt64) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) -[r:studyAt]-> (b:organisation) RETURN r.code ORDER BY a.ID",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    uint64_t uint64Value;
    ASSERT_EQ(lbug_value_get_uint64(&value, &uint64Value), LbugSuccess);
    ASSERT_EQ(uint64Value, 9223372036854775808ull);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_uint64(badValue, &uint64Value), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetInt128) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) -[r:studyAt]-> (b:organisation) RETURN r.hugedata ORDER BY "
               "a.ID",
        &result);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    lbug_int128_t int128;
    ASSERT_EQ(lbug_value_get_int128(&value, &int128), LbugSuccess);
    ASSERT_EQ(int128.high, 100000000);
    ASSERT_EQ(int128.low, 211111111);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_int128(badValue, &int128), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, StringToInt128Test) {
    char input[] = "1844674407370955161811111111";
    lbug_int128_t int128_val;
    ASSERT_EQ(lbug_int128_t_from_string(input, &int128_val), LbugSuccess);
    ASSERT_EQ(int128_val.high, 100000000);
    ASSERT_EQ(int128_val.low, 211111111);

    char badInput[] = "this is not a int128";
    lbug_int128_t int128_val2;
    ASSERT_EQ(lbug_int128_t_from_string(badInput, &int128_val2), LbugError);
}

TEST_F(CApiValueTest, Int128ToStringTest) {
    auto int128_val = lbug_int128_t{211111111, 100000000};
    char* str;
    ASSERT_EQ(lbug_int128_t_to_string(int128_val, &str), LbugSuccess);
    ASSERT_STREQ(str, "1844674407370955161811111111");
    lbug_destroy_string(str);
}

TEST_F(CApiValueTest, GetFloat) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) RETURN a.height ORDER BY a.ID", &result);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    float floatValue;
    ASSERT_EQ(lbug_value_get_float(&value, &floatValue), LbugSuccess);
    ASSERT_FLOAT_EQ(floatValue, 1.731);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_float(badValue, &floatValue), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetDouble) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) RETURN a.eyeSight ORDER BY a.ID", &result);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    double doubleValue;
    ASSERT_EQ(lbug_value_get_double(&value, &doubleValue), LbugSuccess);
    ASSERT_DOUBLE_EQ(doubleValue, 5.0);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_double(badValue, &doubleValue), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetInternalID) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection, (char*)"MATCH (a:person) RETURN a ORDER BY a.ID",
        &result);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    lbug_value nodeIDVal;
    ASSERT_EQ(lbug_value_get_struct_field_value(&value, 0 /* internal ID field idx */, &nodeIDVal),
        LbugSuccess);
    lbug_internal_id_t internalID;
    ASSERT_EQ(lbug_value_get_internal_id(&nodeIDVal, &internalID), LbugSuccess);
    ASSERT_EQ(internalID.table_id, 0);
    ASSERT_EQ(internalID.offset, 0);
    lbug_value_destroy(&nodeIDVal);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_internal_id(badValue, &internalID), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetRelVal) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) -[r:knows]-> (b:person) RETURN r ORDER BY a.ID, b.ID", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value rel;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &rel), LbugSuccess);
    ASSERT_TRUE(rel._is_owned_by_cpp);
    lbug_value relIdVal;
    ASSERT_EQ(lbug_rel_val_get_id_val(&rel, &relIdVal), LbugSuccess);
    lbug_internal_id_t relInternalID;
    ASSERT_EQ(lbug_value_get_internal_id(&relIdVal, &relInternalID), LbugSuccess);
    ASSERT_EQ(relInternalID.table_id, 3);
    ASSERT_EQ(relInternalID.offset, 0);
    lbug_value relSrcIDVal;
    ASSERT_EQ(lbug_rel_val_get_src_id_val(&rel, &relSrcIDVal), LbugSuccess);
    lbug_internal_id_t relSrcID;
    ASSERT_EQ(lbug_value_get_internal_id(&relSrcIDVal, &relSrcID), LbugSuccess);
    ASSERT_EQ(relSrcID.table_id, 0);
    ASSERT_EQ(relSrcID.offset, 0);
    lbug_value relDstIDVal;
    ASSERT_EQ(lbug_rel_val_get_dst_id_val(&rel, &relDstIDVal), LbugSuccess);
    lbug_internal_id_t relDstID;
    ASSERT_EQ(lbug_value_get_internal_id(&relDstIDVal, &relDstID), LbugSuccess);
    ASSERT_EQ(relDstID.table_id, 0);
    ASSERT_EQ(relDstID.offset, 1);
    lbug_value relLabel;
    ASSERT_EQ(lbug_rel_val_get_label_val(&rel, &relLabel), LbugSuccess);
    char* relLabelStr;
    ASSERT_EQ(lbug_value_get_string(&relLabel, &relLabelStr), LbugSuccess);
    ASSERT_STREQ(relLabelStr, "knows");
    uint64_t propertiesSize;
    ASSERT_EQ(lbug_rel_val_get_property_size(&rel, &propertiesSize), LbugSuccess);
    ASSERT_EQ(propertiesSize, 7);
    lbug_destroy_string(relLabelStr);
    lbug_value_destroy(&relLabel);
    lbug_value_destroy(&relIdVal);
    lbug_value_destroy(&relSrcIDVal);
    lbug_value_destroy(&relDstIDVal);
    lbug_value_destroy(&rel);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_rel_val_get_src_id_val(badValue, &relSrcIDVal), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetDate) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) RETURN a.birthdate ORDER BY a.ID", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    lbug_date_t date;
    ASSERT_EQ(lbug_value_get_date(&value, &date), LbugSuccess);
    ASSERT_EQ(date.days, -25567);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_date(badValue, &date), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetTimestamp) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) RETURN a.registerTime ORDER BY a.ID", &result);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    lbug_timestamp_t timestamp;
    ASSERT_EQ(lbug_value_get_timestamp(&value, &timestamp), LbugSuccess);
    ASSERT_EQ(timestamp.value, 1313839530000000);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_timestamp(badValue, &timestamp), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetInterval) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) RETURN a.lastJobDuration ORDER BY a.ID", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    lbug_interval_t interval;
    ASSERT_EQ(lbug_value_get_interval(&value, &interval), LbugSuccess);
    ASSERT_EQ(interval.months, 36);
    ASSERT_EQ(interval.days, 2);
    ASSERT_EQ(interval.micros, 46920000000);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_interval(badValue, &interval), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetString) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) RETURN a.fName ORDER BY a.ID", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    char* str;
    ASSERT_EQ(lbug_value_get_string(&value, &str), LbugSuccess);
    ASSERT_STREQ(str, "Alice");
    lbug_destroy_string(str);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_int32(123);
    ASSERT_EQ(lbug_value_get_string(badValue, &str), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetBlob) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state =
        lbug_connection_query(connection, (char*)R"(RETURN BLOB('\xAA\xBB\xCD\x1A');)", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    uint8_t* blob;
    uint64_t length;
    ASSERT_EQ(lbug_value_get_blob(&value, &blob, &length), LbugSuccess);
    ASSERT_EQ(length, 4);
    ASSERT_EQ(blob[0], 0xAA);
    ASSERT_EQ(blob[1], 0xBB);
    ASSERT_EQ(blob[2], 0xCD);
    ASSERT_EQ(blob[3], 0x1A);
    lbug_destroy_blob(blob);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_blob(badValue, &blob, &length), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetUUID) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)R"(RETURN UUID("A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11");)", &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value value;
    lbug_flat_tuple_get_value(&flatTuple, 0, &value);
    ASSERT_TRUE(value._is_owned_by_cpp);
    ASSERT_FALSE(lbug_value_is_null(&value));
    char* str;
    ASSERT_EQ(lbug_value_get_uuid(&value, &str), LbugSuccess);
    ASSERT_STREQ(str, "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11");
    lbug_destroy_string(str);
    lbug_value_destroy(&value);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_value_get_uuid(badValue, &str), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, ToSting) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) RETURN a.fName, a.isStudent, a.workedHours ORDER BY "
               "a.ID",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));

    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);

    lbug_value value;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &value), LbugSuccess);
    char* str = lbug_value_to_string(&value);
    ASSERT_STREQ(str, "Alice");
    lbug_destroy_string(str);

    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 1, &value), LbugSuccess);
    str = lbug_value_to_string(&value);
    ASSERT_STREQ(str, "True");
    lbug_destroy_string(str);

    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 2, &value), LbugSuccess);
    str = lbug_value_to_string(&value);
    ASSERT_STREQ(str, "[10,5]");
    lbug_destroy_string(str);
    lbug_value_destroy(&value);

    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);
}

TEST_F(CApiValueTest, NodeValGetLabelVal) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection, (char*)"MATCH (a:person) RETURN a ORDER BY a.ID",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));

    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value nodeVal;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &nodeVal), LbugSuccess);
    lbug_value labelVal;
    ASSERT_EQ(lbug_node_val_get_label_val(&nodeVal, &labelVal), LbugSuccess);
    char* labelStr;
    ASSERT_EQ(lbug_value_get_string(&labelVal, &labelStr), LbugSuccess);
    ASSERT_STREQ(labelStr, "person");
    lbug_destroy_string(labelStr);
    lbug_value_destroy(&labelVal);
    lbug_value_destroy(&nodeVal);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_node_val_get_label_val(badValue, &labelVal), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, NodeValGetID) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection, (char*)"MATCH (a:person) RETURN a ORDER BY a.ID",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));

    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value nodeVal;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &nodeVal), LbugSuccess);
    lbug_value nodeIDVal;
    ASSERT_EQ(lbug_node_val_get_id_val(&nodeVal, &nodeIDVal), LbugSuccess);
    ASSERT_NE(nodeIDVal._value, nullptr);
    lbug_internal_id_t internalID;
    ASSERT_EQ(lbug_value_get_internal_id(&nodeIDVal, &internalID), LbugSuccess);
    ASSERT_EQ(internalID.table_id, 0);
    ASSERT_EQ(internalID.offset, 0);
    lbug_value_destroy(&nodeIDVal);
    lbug_value_destroy(&nodeVal);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_node_val_get_id_val(badValue, &nodeIDVal), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, NodeValGetLabelName) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection, (char*)"MATCH (a:person) RETURN a ORDER BY a.ID",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));

    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value nodeVal;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &nodeVal), LbugSuccess);
    lbug_value labelVal;
    ASSERT_EQ(lbug_node_val_get_label_val(&nodeVal, &labelVal), LbugSuccess);
    char* labelStr;
    ASSERT_EQ(lbug_value_get_string(&labelVal, &labelStr), LbugSuccess);
    ASSERT_STREQ(labelStr, "person");
    lbug_destroy_string(labelStr);
    lbug_value_destroy(&labelVal);
    lbug_value_destroy(&nodeVal);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_node_val_get_label_val(badValue, &labelVal), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, NodeValGetProperty) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection, (char*)"MATCH (a:person) RETURN a ORDER BY a.ID",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value node;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &node), LbugSuccess);
    char* propertyName;
    ASSERT_EQ(lbug_node_val_get_property_name_at(&node, 0, &propertyName), LbugSuccess);
    ASSERT_STREQ(propertyName, "ID");
    lbug_destroy_string(propertyName);
    ASSERT_EQ(lbug_node_val_get_property_name_at(&node, 1, &propertyName), LbugSuccess);
    ASSERT_STREQ(propertyName, "fName");
    lbug_destroy_string(propertyName);
    ASSERT_EQ(lbug_node_val_get_property_name_at(&node, 2, &propertyName), LbugSuccess);
    ASSERT_STREQ(propertyName, "gender");
    lbug_destroy_string(propertyName);
    ASSERT_EQ(lbug_node_val_get_property_name_at(&node, 3, &propertyName), LbugSuccess);
    ASSERT_STREQ(propertyName, "isStudent");
    lbug_destroy_string(propertyName);

    lbug_value propertyValue;
    ASSERT_EQ(lbug_node_val_get_property_value_at(&node, 0, &propertyValue), LbugSuccess);
    int64_t propertyValueID;
    ASSERT_EQ(lbug_value_get_int64(&propertyValue, &propertyValueID), LbugSuccess);
    ASSERT_EQ(propertyValueID, 0);
    ASSERT_EQ(lbug_node_val_get_property_value_at(&node, 1, &propertyValue), LbugSuccess);
    char* propertyValuefName;
    ASSERT_EQ(lbug_value_get_string(&propertyValue, &propertyValuefName), LbugSuccess);
    ASSERT_STREQ(propertyValuefName, "Alice");
    lbug_destroy_string(propertyValuefName);
    ASSERT_EQ(lbug_node_val_get_property_value_at(&node, 2, &propertyValue), LbugSuccess);
    int64_t propertyValueGender;
    ASSERT_EQ(lbug_value_get_int64(&propertyValue, &propertyValueGender), LbugSuccess);
    ASSERT_EQ(propertyValueGender, 1);
    ASSERT_EQ(lbug_node_val_get_property_value_at(&node, 3, &propertyValue), LbugSuccess);
    bool propertyValueIsStudent;
    ASSERT_EQ(lbug_value_get_bool(&propertyValue, &propertyValueIsStudent), LbugSuccess);
    ASSERT_EQ(propertyValueIsStudent, true);
    lbug_value_destroy(&propertyValue);

    lbug_value_destroy(&node);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_node_val_get_property_name_at(badValue, 0, &propertyName), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, NodeValToString) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (b:organisation) RETURN b ORDER BY b.ID", &result);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value node;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &node), LbugSuccess);
    ASSERT_TRUE(node._is_owned_by_cpp);

    char* str = lbug_value_to_string(&node);
    ASSERT_STREQ(str,
        "{_ID: 1:0, _LABEL: organisation, ID: 1, name: ABFsUni, orgCode: 325, mark: 3.700000, "
        "score: -2, history: 10 years 5 months 13 hours 24 us, licenseValidInterval: 3 years "
        "5 days, rating: 1.000000, state: {revenue: 138, location: ['toronto','montr,eal'], "
        "stock: {price: [96,56], volume: 1000}}, info: 3.120000}");
    lbug_destroy_string(str);

    lbug_value_destroy(&node);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);
}

TEST_F(CApiValueTest, RelValGetProperty) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) -[e:workAt]-> (b:organisation) RETURN e ORDER BY a.ID, b.ID",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value rel;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &rel), LbugSuccess);
    ASSERT_TRUE(rel._is_owned_by_cpp);
    uint64_t propertiesSize;
    ASSERT_EQ(lbug_rel_val_get_property_size(&rel, &propertiesSize), LbugSuccess);
    ASSERT_EQ(propertiesSize, 3);

    char* propertyName;
    ASSERT_EQ(lbug_rel_val_get_property_name_at(&rel, 0, &propertyName), LbugSuccess);
    ASSERT_STREQ(propertyName, "year");
    lbug_destroy_string(propertyName);

    ASSERT_EQ(lbug_rel_val_get_property_name_at(&rel, 1, &propertyName), LbugSuccess);
    ASSERT_STREQ(propertyName, "grading");
    lbug_destroy_string(propertyName);
    ASSERT_EQ(lbug_rel_val_get_property_name_at(&rel, 2, &propertyName), LbugSuccess);
    ASSERT_STREQ(propertyName, "rating");
    lbug_destroy_string(propertyName);

    lbug_value propertyValue;
    ASSERT_EQ(lbug_rel_val_get_property_value_at(&rel, 0, &propertyValue), LbugSuccess);
    int64_t propertyValueYear;
    ASSERT_EQ(lbug_value_get_int64(&propertyValue, &propertyValueYear), LbugSuccess);
    ASSERT_EQ(propertyValueYear, 2015);

    ASSERT_EQ(lbug_rel_val_get_property_value_at(&rel, 1, &propertyValue), LbugSuccess);
    lbug_value listValue;
    ASSERT_EQ(lbug_value_get_list_element(&propertyValue, 0, &listValue), LbugSuccess);
    double listValueGrading;
    ASSERT_EQ(lbug_value_get_double(&listValue, &listValueGrading), LbugSuccess);
    ASSERT_DOUBLE_EQ(listValueGrading, 3.8);
    ASSERT_EQ(lbug_value_get_list_element(&propertyValue, 1, &listValue), LbugSuccess);
    ASSERT_EQ(lbug_value_get_double(&listValue, &listValueGrading), LbugSuccess);
    ASSERT_DOUBLE_EQ(listValueGrading, 2.5);
    lbug_value_destroy(&listValue);

    ASSERT_EQ(lbug_rel_val_get_property_value_at(&rel, 2, &propertyValue), LbugSuccess);
    float propertyValueRating;
    ASSERT_EQ(lbug_value_get_float(&propertyValue, &propertyValueRating), LbugSuccess);
    ASSERT_FLOAT_EQ(propertyValueRating, 8.2);
    lbug_value_destroy(&propertyValue);

    lbug_value_destroy(&rel);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_rel_val_get_property_name_at(badValue, 0, &propertyName), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, RelValToString) {
    lbug_query_result result;
    lbug_flat_tuple flatTuple;
    lbug_state state;
    auto connection = getConnection();
    state = lbug_connection_query(connection,
        (char*)"MATCH (a:person) -[e:workAt]-> (b:organisation) RETURN e ORDER BY a.ID, b.ID",
        &result);
    ASSERT_EQ(state, LbugSuccess);
    ASSERT_TRUE(lbug_query_result_is_success(&result));
    ASSERT_TRUE(lbug_query_result_has_next(&result));
    state = lbug_query_result_get_next(&result, &flatTuple);
    ASSERT_EQ(state, LbugSuccess);
    lbug_value rel;
    ASSERT_EQ(lbug_flat_tuple_get_value(&flatTuple, 0, &rel), LbugSuccess);
    ASSERT_TRUE(rel._is_owned_by_cpp);
    char* str;
    ASSERT_EQ(lbug_rel_val_to_string(&rel, &str), LbugSuccess);
    ASSERT_STREQ(str, "(0:2)-{_LABEL: workAt, _ID: 7:0, year: 2015, grading: [3.800000,2.500000], "
                      "rating: 8.200000}->(1:1)");
    lbug_destroy_string(str);
    lbug_value_destroy(&rel);
    lbug_flat_tuple_destroy(&flatTuple);
    lbug_query_result_destroy(&result);

    lbug_value* badValue = lbug_value_create_string((char*)"abcdefg");
    ASSERT_EQ(lbug_rel_val_to_string(badValue, &str), LbugError);
    lbug_value_destroy(badValue);
}

TEST_F(CApiValueTest, GetTmFromNonStandardTimestamp) {
    lbug_timestamp_ns_t timestamp_ns = lbug_timestamp_ns_t{17515323532900000};
    lbug_timestamp_ms_t timestamp_ms = lbug_timestamp_ms_t{1012323435341};
    lbug_timestamp_sec_t timestamp_sec = lbug_timestamp_sec_t{1432135648};
    lbug_timestamp_tz_t timestamp_tz = lbug_timestamp_tz_t{771513532900000};
    struct tm tm;
    ASSERT_EQ(lbug_timestamp_ns_to_tm(timestamp_ns, &tm), LbugSuccess);
    ASSERT_EQ(tm.tm_year, 70);
    ASSERT_EQ(tm.tm_mon, 6);
    ASSERT_EQ(tm.tm_mday, 22);
    ASSERT_EQ(tm.tm_hour, 17);
    ASSERT_EQ(tm.tm_min, 22);
    ASSERT_EQ(tm.tm_sec, 3);
    ASSERT_EQ(lbug_timestamp_ms_to_tm(timestamp_ms, &tm), LbugSuccess);
    ASSERT_EQ(tm.tm_year, 102);
    ASSERT_EQ(tm.tm_mon, 0);
    ASSERT_EQ(tm.tm_mday, 29);
    ASSERT_EQ(tm.tm_hour, 16);
    ASSERT_EQ(tm.tm_min, 57);
    ASSERT_EQ(tm.tm_sec, 15);
    ASSERT_EQ(lbug_timestamp_sec_to_tm(timestamp_sec, &tm), LbugSuccess);
    ASSERT_EQ(tm.tm_year, 115);
    ASSERT_EQ(tm.tm_mon, 4);
    ASSERT_EQ(tm.tm_mday, 20);
    ASSERT_EQ(tm.tm_hour, 15);
    ASSERT_EQ(tm.tm_min, 27);
    ASSERT_EQ(tm.tm_sec, 28);
    ASSERT_EQ(lbug_timestamp_tz_to_tm(timestamp_tz, &tm), LbugSuccess);
    ASSERT_EQ(tm.tm_year, 94);
    ASSERT_EQ(tm.tm_mon, 5);
    ASSERT_EQ(tm.tm_mday, 13);
    ASSERT_EQ(tm.tm_hour, 13);
    ASSERT_EQ(tm.tm_min, 18);
    ASSERT_EQ(tm.tm_sec, 52);
}

TEST_F(CApiValueTest, GetTmFromTimestamp) {
    lbug_timestamp_t timestamp = lbug_timestamp_t{171513532900000};
    struct tm tm;
    ASSERT_EQ(lbug_timestamp_to_tm(timestamp, &tm), LbugSuccess);
    ASSERT_EQ(tm.tm_year, 75);
    ASSERT_EQ(tm.tm_mon, 5);
    ASSERT_EQ(tm.tm_mday, 9);
    ASSERT_EQ(tm.tm_hour, 2);
    ASSERT_EQ(tm.tm_min, 38);
    ASSERT_EQ(tm.tm_sec, 52);
}

TEST_F(CApiValueTest, GetTmFromDate) {
    lbug_date_t date = lbug_date_t{-255};
    struct tm tm;
    ASSERT_EQ(lbug_date_to_tm(date, &tm), LbugSuccess);
    ASSERT_EQ(tm.tm_year, 69);
    ASSERT_EQ(tm.tm_mon, 3);
    ASSERT_EQ(tm.tm_mday, 21);
    ASSERT_EQ(tm.tm_hour, 0);
    ASSERT_EQ(tm.tm_min, 0);
    ASSERT_EQ(tm.tm_sec, 0);
}

TEST_F(CApiValueTest, GetTimestampFromTm) {
    struct tm tm;
    tm.tm_year = 75;
    tm.tm_mon = 5;
    tm.tm_mday = 9;
    tm.tm_hour = 2;
    tm.tm_min = 38;
    tm.tm_sec = 52;
    lbug_timestamp_t timestamp;
    ASSERT_EQ(lbug_timestamp_from_tm(tm, &timestamp), LbugSuccess);
    ASSERT_EQ(timestamp.value, 171513532000000);
}

TEST_F(CApiValueTest, GetNonStandardTimestampFromTm) {
    struct tm tm;
    tm.tm_year = 70;
    tm.tm_mon = 6;
    tm.tm_mday = 22;
    tm.tm_hour = 17;
    tm.tm_min = 22;
    tm.tm_sec = 3;
    lbug_timestamp_ns_t timestamp_ns;
    ASSERT_EQ(lbug_timestamp_ns_from_tm(tm, &timestamp_ns), LbugSuccess);
    ASSERT_EQ(timestamp_ns.value, 17515323000000000);
    tm.tm_year = 102;
    tm.tm_mon = 0;
    tm.tm_mday = 29;
    tm.tm_hour = 16;
    tm.tm_min = 57;
    tm.tm_sec = 15;
    lbug_timestamp_ms_t timestamp_ms;
    ASSERT_EQ(lbug_timestamp_ms_from_tm(tm, &timestamp_ms), LbugSuccess);
    ASSERT_EQ(timestamp_ms.value, 1012323435000);
    tm.tm_year = 115;
    tm.tm_mon = 4;
    tm.tm_mday = 20;
    tm.tm_hour = 15;
    tm.tm_min = 27;
    tm.tm_sec = 28;
    lbug_timestamp_sec_t timestamp_sec;
    ASSERT_EQ(lbug_timestamp_sec_from_tm(tm, &timestamp_sec), LbugSuccess);
    ASSERT_EQ(timestamp_sec.value, 1432135648);
    tm.tm_year = 94;
    tm.tm_mon = 5;
    tm.tm_mday = 13;
    tm.tm_hour = 13;
    tm.tm_min = 18;
    tm.tm_sec = 52;
    lbug_timestamp_tz_t timestamp_tz;
    ASSERT_EQ(lbug_timestamp_tz_from_tm(tm, &timestamp_tz), LbugSuccess);
    ASSERT_EQ(timestamp_tz.value, 771513532000000);
}

TEST_F(CApiValueTest, GetDateFromTm) {
    struct tm tm;
    tm.tm_year = 69;
    tm.tm_mon = 3;
    tm.tm_mday = 21;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    lbug_date_t date;
    ASSERT_EQ(lbug_date_from_tm(tm, &date), LbugSuccess);
    ASSERT_EQ(date.days, -255);
}

TEST_F(CApiValueTest, GetDateFromString) {
    char input[] = "1969-04-21";
    lbug_date_t date;
    ASSERT_EQ(lbug_date_from_string(input, &date), LbugSuccess);
    ASSERT_EQ(date.days, -255);

    char badInput[] = "this is not a date";
    ASSERT_EQ(lbug_date_from_string(badInput, &date), LbugError);
}

TEST_F(CApiValueTest, GetStringFromDate) {
    lbug_date_t date = lbug_date_t{-255};
    char* str;
    ASSERT_EQ(lbug_date_to_string(date, &str), LbugSuccess);
    ASSERT_STREQ(str, "1969-04-21");
    lbug_destroy_string(str);
}

TEST_F(CApiValueTest, GetDifftimeFromInterval) {
    lbug_interval_t interval = lbug_interval_t{36, 2, 46920000000};
    double difftime;
    lbug_interval_to_difftime(interval, &difftime);
    ASSERT_DOUBLE_EQ(difftime, 93531720);
}

TEST_F(CApiValueTest, GetIntervalFromDifftime) {
    double difftime = 211110160.479;
    lbug_interval_t interval;
    lbug_interval_from_difftime(difftime, &interval);
    ASSERT_EQ(interval.months, 81);
    ASSERT_EQ(interval.days, 13);
    ASSERT_EQ(interval.micros, 34960479000);
}

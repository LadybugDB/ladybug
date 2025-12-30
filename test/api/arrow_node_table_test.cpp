#include <cstring>
#include <memory>
#include <vector>

#include "api_test/api_test.h"
#include "arrow_test_utils.h"
#include "common/arrow/arrow.h"
#include "gtest/gtest.h"
#include "storage/table/arrow_node_table.h"

using namespace lbug;
using namespace lbug::storage;

class ArrowNodeTableTest : public lbug::testing::ApiTest {
protected:
};

TEST_F(ArrowNodeTableTest, CreateArrowTableFromVectors) {
    // Create test data
    std::vector<int32_t> intData = {1, 2, 3, 4, 5};
    std::vector<std::string> stringData = {"a", "b", "c", "d", "e"};

    // Create Arrow schema with 2 fields
    ArrowSchema schema;
    createStructSchema(&schema, 2);
    createSchema<int32_t>(schema.children[0], "int_col");
    createSchema<std::string>(schema.children[1], "string_col");

    // Create Arrow array with 2 children
    ArrowArray array;
    array.length = intData.size();
    array.null_count = 0;
    array.offset = 0;
    array.n_buffers = 1;
    array.n_children = 2;
    array.buffers = static_cast<const void**>(malloc(sizeof(void*)));
    array.buffers[0] = nullptr;
    array.children = static_cast<ArrowArray**>(malloc(sizeof(ArrowArray*) * 2));
    for (int i = 0; i < 2; i++) {
        array.children[i] = static_cast<ArrowArray*>(malloc(sizeof(ArrowArray)));
    }
    createInt32Array(array.children[0], intData);
    createStringArray(array.children[1], stringData);
    array.dictionary = nullptr;
    array.release = [](ArrowArray* arr) {
        if (arr->children) {
            for (int64_t i = 0; i < arr->n_children; i++) {
                if (arr->children[i]->release) {
                    arr->children[i]->release(arr->children[i]);
                }
                free(arr->children[i]);
            }
            free(arr->children);
        }
        if (arr->buffers) {
            free(const_cast<void**>(arr->buffers));
        }
        arr->release = nullptr;
    };
    array.private_data = nullptr;

    // Verify properties
    EXPECT_EQ(array.length, 5);
    EXPECT_EQ(array.n_children, 2);
    EXPECT_STREQ(schema.children[0]->name, "int_col");
    EXPECT_STREQ(schema.children[1]->name, "string_col");

    // Cleanup
    if (schema.release)
        schema.release(&schema);
    if (array.release)
        array.release(&array);
}

TEST_F(ArrowNodeTableTest, ArrowTableTypeConversions) {
    // Test various data types
    std::vector<int64_t> int64Data = {1000000000LL, 2000000000LL, 3000000000LL};
    std::vector<double> doubleData = {1.1, 2.2, 3.3};
    std::vector<bool> boolData = {true, false, true};

    // Create Arrow schema with 3 fields
    ArrowSchema schema;
    createStructSchema(&schema, 3);
    createSchema<int64_t>(schema.children[0], "int64_col");
    createSchema<double>(schema.children[1], "double_col");
    createSchema<bool>(schema.children[2], "bool_col");

    // Create Arrow array with 3 children
    ArrowArray array;
    array.length = int64Data.size();
    array.null_count = 0;
    array.offset = 0;
    array.n_buffers = 1;
    array.n_children = 3;
    array.buffers = static_cast<const void**>(malloc(sizeof(void*)));
    array.buffers[0] = nullptr;
    array.children = static_cast<ArrowArray**>(malloc(sizeof(ArrowArray*) * 3));
    for (int i = 0; i < 3; i++) {
        array.children[i] = static_cast<ArrowArray*>(malloc(sizeof(ArrowArray)));
    }
    createInt64Array(array.children[0], int64Data);
    createDoubleArray(array.children[1], doubleData);
    createBoolArray(array.children[2], boolData);
    array.dictionary = nullptr;
    array.release = [](ArrowArray* arr) {
        if (arr->children) {
            for (int64_t i = 0; i < arr->n_children; i++) {
                if (arr->children[i]->release) {
                    arr->children[i]->release(arr->children[i]);
                }
                free(arr->children[i]);
            }
            free(arr->children);
        }
        if (arr->buffers) {
            free(const_cast<void**>(arr->buffers));
        }
        arr->release = nullptr;
    };
    array.private_data = nullptr;

    // Verify properties
    EXPECT_EQ(array.length, 3);
    EXPECT_EQ(array.n_children, 3);

    // Verify format strings (types)
    EXPECT_STREQ(schema.children[0]->format, "l"); // int64
    EXPECT_STREQ(schema.children[1]->format, "g"); // double
    EXPECT_STREQ(schema.children[2]->format, "b"); // bool

    // Cleanup
    if (schema.release)
        schema.release(&schema);
    if (array.release)
        array.release(&array);
}

TEST_F(ArrowNodeTableTest, EmptyArrowTable) {
    // Create empty data
    std::vector<int32_t> emptyData;

    // Create Arrow schema
    ArrowSchema schema;
    createStructSchema(&schema, 1);
    createSchema<int32_t>(schema.children[0], "col");

    // Create Arrow array
    ArrowArray array;
    array.length = 0;
    array.null_count = 0;
    array.offset = 0;
    array.n_buffers = 1;
    array.n_children = 1;
    array.buffers = static_cast<const void**>(malloc(sizeof(void*)));
    array.buffers[0] = nullptr;
    array.children = static_cast<ArrowArray**>(malloc(sizeof(ArrowArray*)));
    array.children[0] = static_cast<ArrowArray*>(malloc(sizeof(ArrowArray)));
    createInt32Array(array.children[0], emptyData);
    array.dictionary = nullptr;
    array.release = [](ArrowArray* arr) {
        if (arr->children) {
            for (int64_t i = 0; i < arr->n_children; i++) {
                if (arr->children[i]->release) {
                    arr->children[i]->release(arr->children[i]);
                }
                free(arr->children[i]);
            }
            free(arr->children);
        }
        if (arr->buffers) {
            free(const_cast<void**>(arr->buffers));
        }
        arr->release = nullptr;
    };
    array.private_data = nullptr;

    // Verify empty table properties
    EXPECT_EQ(array.length, 0);
    EXPECT_EQ(array.n_children, 1);
    EXPECT_EQ(array.children[0]->length, 0);

    // Cleanup
    if (schema.release)
        schema.release(&schema);
    if (array.release)
        array.release(&array);
}

TEST_F(ArrowNodeTableTest, ArrowTableLargeData) {
    // Test with larger dataset
    const size_t largeSize = 10000;
    std::vector<int32_t> largeData(largeSize);
    for (size_t i = 0; i < largeSize; i++) {
        largeData[i] = static_cast<int32_t>(i);
    }

    // Create Arrow schema
    ArrowSchema schema;
    createStructSchema(&schema, 1);
    createSchema<int32_t>(schema.children[0], "col");

    // Create Arrow array
    ArrowArray array;
    array.length = largeSize;
    array.null_count = 0;
    array.offset = 0;
    array.n_buffers = 1;
    array.n_children = 1;
    array.buffers = static_cast<const void**>(malloc(sizeof(void*)));
    array.buffers[0] = nullptr;
    array.children = static_cast<ArrowArray**>(malloc(sizeof(ArrowArray*)));
    array.children[0] = static_cast<ArrowArray*>(malloc(sizeof(ArrowArray)));
    createInt32Array(array.children[0], largeData);
    array.dictionary = nullptr;
    array.release = [](ArrowArray* arr) {
        if (arr->children) {
            for (int64_t i = 0; i < arr->n_children; i++) {
                if (arr->children[i]->release) {
                    arr->children[i]->release(arr->children[i]);
                }
                free(arr->children[i]);
            }
            free(arr->children);
        }
        if (arr->buffers) {
            free(const_cast<void**>(arr->buffers));
        }
        arr->release = nullptr;
    };
    array.private_data = nullptr;

    // Verify table properties
    EXPECT_EQ(array.length, largeSize);
    EXPECT_EQ(array.n_children, 1);

    // Verify data integrity (spot check)
    auto* data = static_cast<const int32_t*>(array.children[0]->buffers[1]);
    EXPECT_EQ(data[0], 0);
    EXPECT_EQ(data[100], 100);
    EXPECT_EQ(data[largeSize - 1], static_cast<int32_t>(largeSize - 1));

    // Cleanup
    if (schema.release)
        schema.release(&schema);
    if (array.release)
        array.release(&array);
}

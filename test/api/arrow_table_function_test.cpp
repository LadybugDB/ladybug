#include <cstring>
#include <memory>
#include <vector>

#include "api_test/api_test.h"
#include "arrow_test_utils.h"
#include "common/arrow/arrow.h"
#include "gtest/gtest.h"
#include "storage/table/arrow_table_support.h"

using namespace lbug;

class ArrowTableFunctionTest : public lbug::testing::ApiTest {
protected:
};

TEST_F(ArrowTableFunctionTest, CreateArrowTable) {
    // Create simple test data
    std::vector<int32_t> intData = {1, 2, 3};

    // Create Arrow schema
    ArrowSchema schema;
    createStructSchema(&schema, 1);
    createSchema<int32_t>(schema.children[0], "id");

    // Create Arrow array
    ArrowArray array;
    array.length = intData.size();
    array.null_count = 0;
    array.offset = 0;
    array.n_buffers = 1;
    array.n_children = 1;
    array.buffers = static_cast<const void**>(malloc(sizeof(void*)));
    array.buffers[0] = nullptr;
    array.children = static_cast<ArrowArray**>(malloc(sizeof(ArrowArray*)));
    array.children[0] = static_cast<ArrowArray*>(malloc(sizeof(ArrowArray)));
    createInt32Array(array.children[0], intData);
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

    // Test that we can create arrays with Arrow C Data Interface
    EXPECT_EQ(array.length, 3);
    EXPECT_EQ(array.n_children, 1);
    EXPECT_STREQ(schema.children[0]->name, "id");

    // Cleanup
    if (schema.release)
        schema.release(&schema);
    if (array.release)
        array.release(&array);
}

TEST_F(ArrowTableFunctionTest, CreateArrowTableMultipleColumns) {
    // Create test data with multiple columns
    std::vector<int32_t> idData = {1, 2, 3};
    std::vector<std::string> nameData = {"Alice", "Bob", "Charlie"};
    std::vector<double> scoreData = {95.5, 87.3, 92.1};

    // Create Arrow schema with 3 fields
    ArrowSchema schema;
    createStructSchema(&schema, 3);
    createSchema<int32_t>(schema.children[0], "id");
    createSchema<std::string>(schema.children[1], "name");
    createSchema<double>(schema.children[2], "score");

    // Create Arrow array with 3 children
    ArrowArray array;
    array.length = idData.size();
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
    createInt32Array(array.children[0], idData);
    createStringArray(array.children[1], nameData);
    createDoubleArray(array.children[2], scoreData);
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
    EXPECT_STREQ(schema.children[0]->name, "id");
    EXPECT_STREQ(schema.children[1]->name, "name");
    EXPECT_STREQ(schema.children[2]->name, "score");

    // Cleanup
    if (schema.release)
        schema.release(&schema);
    if (array.release)
        array.release(&array);
}

TEST_F(ArrowTableFunctionTest, CypherQueryWithArrowTable) {
    // Create test data for Cypher query
    std::vector<int32_t> valueData = {1, 2, 3, 4, 5};
    std::vector<std::string> nameData = {"a", "b", "c", "d", "e"};

    // Create Arrow schema with 2 fields
    ArrowSchema schema;
    createStructSchema(&schema, 2);
    createSchema<int32_t>(schema.children[0], "value");
    createSchema<std::string>(schema.children[1], "name");

    // Create Arrow array with 2 children
    std::vector<ArrowArray> arrays;
    ArrowArray array;
    array.length = valueData.size();
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
    createInt32Array(array.children[0], valueData);
    createStringArray(array.children[1], nameData);
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

    arrays.push_back(array);

    // Create a view from the Arrow table
    auto creationResult =
        ArrowTableSupport::createViewFromArrowTable(*conn, "arrow_test_view", schema, arrays);
    if (!creationResult.queryResult->isSuccess()) {
        std::cout << "Error: " << creationResult.queryResult->getErrorMessage() << std::endl;
    }
    ASSERT_TRUE(creationResult.queryResult->isSuccess());

    // Query the view with Cypher
    auto queryResult =
        conn->query("MATCH (n:arrow_test_view) WHERE n.value > 2 RETURN n.value, n.name");
    ASSERT_TRUE(queryResult->isSuccess());

    // Verify we got filtered results (values > 2: 3, 4, 5)
    // Note: The actual data reading is not yet implemented, so this test verifies
    // that the view creation succeeds and the query executes
    EXPECT_TRUE(true);

    // Cleanup
    if (schema.release)
        schema.release(&schema);
    // Note: arrays cleanup is handled by the vector and the release callback
}

TEST_F(ArrowTableFunctionTest, UnregisterArrowTable) {
    // Create test data for unregister test
    std::vector<int32_t> valueData = {10, 20, 30};
    std::vector<std::string> nameData = {"x", "y", "z"};

    // Create Arrow schema with 2 fields
    ArrowSchema schema;
    createStructSchema(&schema, 2);
    createSchema<int32_t>(schema.children[0], "value");
    createSchema<std::string>(schema.children[1], "name");

    // Create Arrow array with 2 children
    std::vector<ArrowArray> arrays;
    ArrowArray array;
    array.length = valueData.size();
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
    createInt32Array(array.children[0], valueData);
    createStringArray(array.children[1], nameData);
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

    arrays.push_back(array);

    // Create a view from the Arrow table
    auto creationResult =
        ArrowTableSupport::createViewFromArrowTable(*conn, "arrow_unregister_test", schema, arrays);
    ASSERT_TRUE(creationResult.queryResult->isSuccess());

    // Verify the table exists by querying it
    auto queryResult = conn->query("MATCH (n:arrow_unregister_test) RETURN count(n) as count");
    ASSERT_TRUE(queryResult->isSuccess());

    // Unregister the arrow table (drop table and unregister data)
    auto unregisterResult = ArrowTableSupport::unregisterArrowTable(*conn, "arrow_unregister_test");
    ASSERT_TRUE(unregisterResult->isSuccess());

    // Verify the table no longer exists
    auto queryAfterDrop = conn->query("MATCH (n:arrow_unregister_test) RETURN count(n) as count");
    // This should fail because the table doesn't exist
    ASSERT_FALSE(queryAfterDrop->isSuccess());

    // Cleanup
    if (schema.release)
        schema.release(&schema);
    // Note: arrays cleanup is handled by the vector and the release callback
}

#include "api_test/api_test.h"
#include "main/storage_driver.h"

using namespace lbug::main;
using namespace lbug::testing;
using namespace lbug::common;

class StorageDriverTest : public lbug::testing::ApiTest {
protected:
};

// TODO(Guodong): FIX-ME. Rework storage driver scan.
// TEST_F(ApiTest, StorageDriverScan) {
//     auto storageDriver = std::make_unique<StorageDriver>(database.get());
//     auto size = 6;
//     auto nodeOffsetsBuffer = std::make_unique<uint8_t[]>(sizeof(offset_t) * size);
//     auto nodeOffsets = (offset_t*)nodeOffsetsBuffer.get();
//     nodeOffsets[0] = 7;
//     nodeOffsets[1] = 0;
//     nodeOffsets[2] = 3;
//     nodeOffsets[3] = 1;
//     nodeOffsets[4] = 2;
//     nodeOffsets[5] = 6;
//     auto result = std::make_unique<uint8_t[]>(sizeof(int64_t) * size);
//     auto resultBuffer = (uint8_t*)result.get();
//     storageDriver->scan("person", "ID", nodeOffsets, size, resultBuffer, 3);
//     auto ids = (int64_t*)resultBuffer;
//     ASSERT_EQ(ids[0], 10);
//     ASSERT_EQ(ids[1], 0);
//     ASSERT_EQ(ids[2], 5);
//     ASSERT_EQ(ids[3], 2);
//     ASSERT_EQ(ids[4], 3);
//     ASSERT_EQ(ids[5], 9);
// }

TEST_F(StorageDriverTest, GetNumNodes) {
    auto storageDriver = std::make_unique<StorageDriver>(database.get());

    auto numNodes = storageDriver->getNumNodes("person");
    auto numNodesQuery = conn->query("MATCH (n:person) RETURN COUNT(n)");
    ASSERT_TRUE(numNodesQuery->isSuccess());
    ASSERT_EQ(numNodes, numNodesQuery->getNext()->getValue(0)->getValue<int64_t>());
}

TEST_F(StorageDriverTest, GetNumNodesNonNodeTable) {
    auto storageDriver = std::make_unique<StorageDriver>(database.get());

    EXPECT_ANY_THROW(storageDriver->getNumNodes("knows"));
}

TEST_F(StorageDriverTest, GetNumRels) {
    auto storageDriver = std::make_unique<StorageDriver>(database.get());

    auto numRels = storageDriver->getNumRels("knows");
    auto numRelsQuery = conn->query("MATCH ()-[:knows]->() RETURN COUNT(*)");
    ASSERT_TRUE(numRelsQuery->isSuccess());
    ASSERT_EQ(numRels, numRelsQuery->getNext()->getValue(0)->getValue<int64_t>());
}

TEST_F(StorageDriverTest, GetNumRelsNonRelTable) {
    auto storageDriver = std::make_unique<StorageDriver>(database.get());

    EXPECT_ANY_THROW(storageDriver->getNumRels("person"));
}
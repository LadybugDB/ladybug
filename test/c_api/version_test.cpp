#include <filesystem>
#include <fstream>

#include "c_api/lbug.h"
#include "c_api_test/c_api_test.h"
#include "gtest/gtest.h"

using namespace lbug::main;
using namespace lbug::testing;
using namespace lbug::common;

class CApiVersionTest : public CApiTest {
public:
    std::string getInputDir() override {
        return TestHelper::appendLbugRootPath("dataset/tinysnb/");
    }

    void TearDown() override { APIDBTest::TearDown(); }
};

class EmptyCApiVersionTest : public CApiVersionTest {
public:
    std::string getInputDir() override { return "empty"; }
};

TEST_F(EmptyCApiVersionTest, GetVersion) {
    lbug_connection_destroy(&connection);
    lbug_database_destroy(&_database);
    auto version = lbug_get_version();
    ASSERT_NE(version, nullptr);
    ASSERT_STREQ(version, LBUG_CMAKE_VERSION);
    lbug_destroy_string(version);
}

TEST_F(CApiVersionTest, GetStorageVersion) {
    auto storageVersion = lbug_get_storage_version();
    if (inMemMode) {
        GTEST_SKIP();
    }
    // Reset the database to ensure that the lock on db file is released.
    lbug_connection_destroy(&connection);
    lbug_database_destroy(&_database);
    auto data = std::filesystem::path(databasePath);
    std::ifstream dbFile;
    dbFile.open(data, std::ios::binary);
    ASSERT_TRUE(dbFile.is_open());
    char magic[5];
    dbFile.read(magic, 4);
    magic[4] = '\0';
    ASSERT_STREQ(magic, "LBUG");
    uint64_t actualVersion;
    dbFile.read(reinterpret_cast<char*>(&actualVersion), sizeof(actualVersion));
    dbFile.close();
    ASSERT_EQ(storageVersion, actualVersion);
}

TEST_F(EmptyCApiVersionTest, GetStorageVersion) {
    auto storageVersion = lbug_get_storage_version();
    if (inMemMode) {
        GTEST_SKIP();
    }
    // Reset the database to ensure that the lock on db file is released.
    lbug_connection_destroy(&connection);
    lbug_database_destroy(&_database);
    auto data = std::filesystem::path(databasePath);
    std::ifstream dbFile;
    dbFile.open(data, std::ios::binary);
    ASSERT_TRUE(dbFile.is_open());
    char magic[5];
    dbFile.read(magic, 4);
    magic[4] = '\0';
    ASSERT_STREQ(magic, "LBUG");
    uint64_t actualVersion;
    dbFile.read(reinterpret_cast<char*>(&actualVersion), sizeof(actualVersion));
    dbFile.close();
    ASSERT_EQ(storageVersion, actualVersion);
}

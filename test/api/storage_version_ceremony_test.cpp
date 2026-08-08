#include <fstream>

#include "api_test/api_test.h"
#include "common/exception/runtime.h"
#include "common/file_system/local_file_system.h"
#include "common/serializer/buffered_file.h"
#include "common/serializer/serializer.h"
#include "common/system_config.h"
#include "storage/database_header.h"
#include "storage/storage_version_info.h"
#include <format>

using namespace lbug::common;
using namespace lbug::main;
using namespace lbug::storage;
using namespace lbug::testing;

class StorageVersionCeremonyTest : public ApiTest {
    void SetUp() override { BaseGraphTest::SetUp(); }
};

TEST(StorageVersionMappingTest, KnownVersionsMap) {
    ASSERT_EQ(StorageVersionInfo::getStorageVersionForVersionString("0.17.0"),
        StorageVersionInfo::STORAGE_VERSION_41);
    ASSERT_EQ(StorageVersionInfo::getStorageVersionForVersionString("0.19.1"),
        StorageVersionInfo::STORAGE_VERSION_43);
    // 0.13.7 is not in the map but matches the 0.12.x-0.16.x storage-version-40 family.
    ASSERT_EQ(StorageVersionInfo::getStorageVersionForVersionString("0.13.7"),
        StorageVersionInfo::STORAGE_VERSION_40);
    // 4-component nightly strings are truncated to 3 components before lookup.
    ASSERT_EQ(StorageVersionInfo::getStorageVersionForVersionString("0.17.0.1"),
        StorageVersionInfo::STORAGE_VERSION_41);
}

TEST(StorageVersionMappingTest, CurrentBuildVersionIsMapped) {
    // Guards the throw-on-unknown change: the version this build was compiled with must always
    // have a mapping, otherwise every open would throw.
    ASSERT_NO_THROW(StorageVersionInfo::getStorageVersion());
}

TEST(StorageVersionMappingTest, UnknownVersionThrows) {
    EXPECT_THROW(StorageVersionInfo::getStorageVersionForVersionString("0.99.0"), RuntimeException);
    EXPECT_THROW(StorageVersionInfo::getStorageVersionForVersionString("garbage"),
        RuntimeException);
}

static void writeStorageVersionToHeader(const std::string& databasePath, uint64_t storageVersion) {
    auto localFileSystem = LocalFileSystem("");
    auto fileInfo = localFileSystem.openFile(databasePath,
        FileOpenFlags(FileFlags::READ_ONLY | FileFlags::WRITE));
    auto databaseHeader = DatabaseHeader::readDatabaseHeader(*fileInfo);
    ASSERT_TRUE(databaseHeader.has_value());
    databaseHeader->storageVersion = storageVersion;
    auto writer = std::make_shared<BufferedFileWriter>(*fileInfo);
    Serializer serializer{writer};
    databaseHeader->serialize(serializer);
    writer->flush();
    writer->sync();
}

TEST_F(StorageVersionCeremonyTest, UnreadableStorageVersionRefusedOnOpen) {
    if (databasePath.empty() || databasePath == ":memory:") {
        return;
    }
    // Closing the database checkpoints (forceCheckpointOnClose defaults true), writing the header.
    { auto db = std::make_unique<Database>(databasePath, *systemConfig); }
    writeStorageVersionToHeader(databasePath, 9999);
    try {
        auto db = std::make_unique<Database>(databasePath, *systemConfig);
        FAIL() << "Expected RuntimeException on open";
    } catch (const RuntimeException& e) {
        ASSERT_EQ(std::string(e.what()),
            std::format("Runtime exception: Trying to read a database file with a different "
                        "version. Database file version: 9999, Current build storage version: {}",
                StorageVersionInfo::getStorageVersion()));
    }
}

TEST_F(StorageVersionCeremonyTest, ReadDatabaseHeaderPropagatesVersionMismatch) {
    if (databasePath.empty() || databasePath == ":memory:") {
        return;
    }
    { auto db = std::make_unique<Database>(databasePath, *systemConfig); }
    writeStorageVersionToHeader(databasePath, 9999);
    auto localFileSystem = LocalFileSystem("");
    auto fileInfo = localFileSystem.openFile(databasePath, FileOpenFlags(FileFlags::READ_ONLY));
    EXPECT_THROW(DatabaseHeader::readDatabaseHeader(*fileInfo), RuntimeException);
}

TEST_F(StorageVersionCeremonyTest, ReadDatabaseHeaderNoMagicIsNoHeader) {
    if (databasePath.empty() || databasePath == ":memory:") {
        return;
    }
    // Create the database file first so the file and its parent directory exist, then overwrite
    // it below (std::ofstream with the default openmode truncates).
    { auto db = std::make_unique<Database>(databasePath, *systemConfig); }
    // A page-sized file with no magic bytes is the optimistic-pre-checkpoint-write case and must
    // still read as "no header", not throw.
    {
        std::ofstream file(databasePath, std::ios::binary);
        std::vector<char> zeros(LBUG_PAGE_SIZE, 0);
        file.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    auto localFileSystem = LocalFileSystem("");
    auto fileInfo = localFileSystem.openFile(databasePath, FileOpenFlags(FileFlags::READ_ONLY));
    ASSERT_FALSE(DatabaseHeader::readDatabaseHeader(*fileInfo).has_value());
}

TEST_F(StorageVersionCeremonyTest, CheckpointRefusesUpgradeWhenDisallowed) {
    if (databasePath.empty() || databasePath == ":memory:") {
        return;
    }
    { auto db = std::make_unique<Database>(databasePath, *systemConfig); }
    writeStorageVersionToHeader(databasePath, StorageVersionInfo::STORAGE_VERSION_41);
    auto db = std::make_unique<Database>(databasePath, *systemConfig);
    auto con = std::make_unique<Connection>(db.get());
    ASSERT_TRUE(con->query("CALL allow_storage_version_upgrade=false;")->isSuccess());
    auto result = con->query("CHECKPOINT;");
    ASSERT_FALSE(result->isSuccess());
    ASSERT_EQ(result->toString(),
        std::format("Runtime exception: Checkpoint would upgrade the database storage version "
                    "from {} to {}, after which older Lbug binaries can no longer open this "
                    "database file, and allow_storage_version_upgrade is false. Run CALL "
                    "allow_storage_version_upgrade=true; to allow the upgrade.",
            StorageVersionInfo::STORAGE_VERSION_41, StorageVersionInfo::getStorageVersion()));
    ASSERT_TRUE(con->query("CALL allow_storage_version_upgrade=true;")->isSuccess());
    ASSERT_TRUE(con->query("CHECKPOINT;")->isSuccess());
    auto versionResult = con->query("CALL storage_version() RETURN *;");
    ASSERT_TRUE(versionResult->isSuccess());
    ASSERT_EQ(TestHelper::convertResultToString(*versionResult),
        std::vector<std::string>{std::to_string(StorageVersionInfo::getStorageVersion())});
}

#include <filesystem>
#include <string>

#include "common/exception/copy.h"
#include "common/exception/io.h"
#include "common/exception/runtime.h"
#include "common/file_system/virtual_file_system.h"
#include "gtest/gtest.h"
#include "storage/table/ice_disk_utils.h"
#include "test_helper/test_helper.h"

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::testing;

static const std::string FIXTURES_DIR =
    TestHelper::appendLbugRootPath("dataset/ice-disk-test/fixtures");
static const std::string DEMO_DB_ICEBUG_DISK =
    TestHelper::appendLbugRootPath("dataset/demo-db/icebug-disk");

// ─────────────────────────────────────────────────────────────
// getBasePath
// ─────────────────────────────────────────────────────────────

TEST(IceDiskUtils_GetBasePath, PrefixOnly) {
    EXPECT_EQ("", IceDiskUtils::getBasePath("icebug-disk"));
}

TEST(IceDiskUtils_GetBasePath, PrefixWithColonOnly) {
    EXPECT_EQ("", IceDiskUtils::getBasePath("icebug-disk:"));
}

TEST(IceDiskUtils_GetBasePath, PrefixWithAbsolutePath) {
    EXPECT_EQ("/some/path", IceDiskUtils::getBasePath("icebug-disk:/some/path"));
}

TEST(IceDiskUtils_GetBasePath, PrefixWithRelativePath) {
    EXPECT_EQ("rel/path", IceDiskUtils::getBasePath("icebug-disk:rel/path"));
}

TEST(IceDiskUtils_GetBasePath, PrefixWithDot) {
    EXPECT_EQ(".", IceDiskUtils::getBasePath("icebug-disk:."));
}

// ─────────────────────────────────────────────────────────────
// joinPath
// ─────────────────────────────────────────────────────────────
TEST(IceDiskUtils_JoinPath, EmptyBase) {
    EXPECT_EQ("file.parquet", IceDiskUtils::joinPath("", "file.parquet"));
}

TEST(IceDiskUtils_JoinPath, BaseWithoutTrailingSlash) {
    EXPECT_EQ("/base/file.parquet", IceDiskUtils::joinPath("/base", "file.parquet"));
}

TEST(IceDiskUtils_JoinPath, BaseWithTrailingSlash) {
    EXPECT_EQ("/base/file.parquet", IceDiskUtils::joinPath("/base/", "file.parquet"));
}

TEST(IceDiskUtils_JoinPath, BaseWithBackslash) {
    EXPECT_EQ("base\\file.parquet", IceDiskUtils::joinPath("base\\", "file.parquet"));
}

// ─────────────────────────────────────────────────────────────
// constructNodeTablePath
// ─────────────────────────────────────────────────────────────
TEST(IceDiskUtils_ConstructNodeTablePath, EmptyDir) {
    EXPECT_EQ("nodes_city.parquet", IceDiskUtils::constructNodeTablePath("", "city", ".parquet"));
}

TEST(IceDiskUtils_ConstructNodeTablePath, WithDir) {
    EXPECT_EQ("/some/dir/nodes_user.parquet",
        IceDiskUtils::constructNodeTablePath("/some/dir", "user", ".parquet"));
}

// ─────────────────────────────────────────────────────────────
// constructCSRPaths
// ─────────────────────────────────────────────────────────────
TEST(IceDiskUtils_ConstructCSRPaths, EmptyDir) {
    auto paths = IceDiskUtils::constructCSRPaths("", "follows", ".parquet");
    EXPECT_EQ("indices_follows.parquet", paths.indices);
    EXPECT_EQ("indptr_follows.parquet", paths.indptr);
}

TEST(IceDiskUtils_ConstructCSRPaths, WithDir) {
    auto paths = IceDiskUtils::constructCSRPaths("/some/dir", "knows", ".parquet");
    EXPECT_EQ("/some/dir/indices_knows.parquet", paths.indices);
    EXPECT_EQ("/some/dir/indptr_knows.parquet", paths.indptr);
}

// ─────────────────────────────────────────────────────────────
// resolveIceDiskPath
// ─────────────────────────────────────────────────────────────
TEST(IceDiskUtils_ResolveIceDiskPath, AbsolutePathUnchanged) {
    EXPECT_EQ("/abs/path/file.parquet",
        IceDiskUtils::resolveIceDiskPath("/abs/path/file.parquet", "/dbdir"));
}

TEST(IceDiskUtils_ResolveIceDiskPath, RelativePathJoinedWithDbDir) {
    auto result = IceDiskUtils::resolveIceDiskPath("rel/file.parquet", "/dbdir");
    // std::filesystem normalizes the path, so check it ends with rel/file.parquet
    EXPECT_TRUE(result.find("rel/file.parquet") != std::string::npos);
    EXPECT_TRUE(std::filesystem::path(result).is_absolute());
}

TEST(IceDiskUtils_ResolveIceDiskPath, EmptyPathUsesDbDir) {
    auto result = IceDiskUtils::resolveIceDiskPath("", "/dbdir");
    EXPECT_TRUE(std::filesystem::path(result).is_absolute());
}

TEST(IceDiskUtils_ResolveIceDiskPath, DotPathNormalized) {
    auto result = IceDiskUtils::resolveIceDiskPath(".", "/dbdir");
    // "." relative to /dbdir should resolve to /dbdir (with possible trailing slash on some impls)
    EXPECT_TRUE(result == "/dbdir" || result == "/dbdir/");
}

// ─────────────────────────────────────────────────────────────
// checkVersionCompatibility
// ─────────────────────────────────────────────────────────────
class IceDiskCheckVersionTest : public ::testing::Test {
protected:
    VirtualFileSystem vfs;
    const std::string dbDir = FIXTURES_DIR;
};

TEST_F(IceDiskCheckVersionTest, NullVfs) {
    EXPECT_THROW(IceDiskUtils::checkVersionCompatibility(nullptr, dbDir,
                     DEMO_DB_ICEBUG_DISK + "/nodes_person.parquet"),
        RuntimeException);
}

TEST_F(IceDiskCheckVersionTest, FileDoesNotExist) {
    EXPECT_THROW(IceDiskUtils::checkVersionCompatibility(&vfs, dbDir,
                     FIXTURES_DIR + "/nodes_nonexistent.parquet"),
        IOException);
}

TEST_F(IceDiskCheckVersionTest, NotAParquetFile) {
    EXPECT_THROW(IceDiskUtils::checkVersionCompatibility(&vfs, dbDir,
                     FIXTURES_DIR + "/nodes_notparquet.parquet"),
        CopyException);
}

TEST_F(IceDiskCheckVersionTest, MissingVersionKey) {
    try {
        IceDiskUtils::checkVersionCompatibility(&vfs, dbDir,
            FIXTURES_DIR + "/nodes_noversion.parquet");
        FAIL() << "Expected RuntimeException for missing version key";
    } catch (const RuntimeException& e) {
        EXPECT_TRUE(std::string(e.what()).find("missing icebug_disk_version") != std::string::npos);
    }
}

TEST_F(IceDiskCheckVersionTest, WrongVersionValue) {
    try {
        IceDiskUtils::checkVersionCompatibility(&vfs, dbDir,
            FIXTURES_DIR + "/nodes_wrongversion.parquet");
        FAIL() << "Expected RuntimeException for wrong version";
    } catch (const RuntimeException& e) {
        EXPECT_TRUE(std::string(e.what()).find("does not support icebug_disk_version: v99") !=
                    std::string::npos);
    }
}

TEST_F(IceDiskCheckVersionTest, UppercaseVersionSucceeds) {
    // "V1" should match "v1" case-insensitively
    EXPECT_NO_THROW(IceDiskUtils::checkVersionCompatibility(&vfs, dbDir,
        FIXTURES_DIR + "/nodes_upperversion.parquet"));
}

TEST_F(IceDiskCheckVersionTest, ValidV1Succeeds) {
    EXPECT_NO_THROW(IceDiskUtils::checkVersionCompatibility(&vfs, dbDir,
        DEMO_DB_ICEBUG_DISK + "/nodes_user.parquet"));
}

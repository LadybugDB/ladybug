#include <filesystem>
#include <string>

#include "common/exception/copy.h"
#include "common/exception/io.h"
#include "common/exception/runtime.h"
#include "common/file_system/virtual_file_system.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "main/client_context.h"
#include "main/connection.h"
#include "main/database.h"
#include "storage/table/ice_disk_utils.h"
#include "test_helper/test_helper.h"

using namespace lbug::common;
using namespace lbug::main;
using namespace lbug::storage;
using namespace lbug::testing;

static const std::string DATASET_DIR = TestHelper::appendLbugRootPath("dataset/ice-disk-test/");

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

TEST(IceDiskUtils_JoinPath, S3URI) {
    EXPECT_EQ("s3://bucket/prefix/file.parquet",
        IceDiskUtils::joinPath("s3://bucket/prefix", "file.parquet"));
}

TEST(IceDiskUtils_JoinPath, HttpsURI) {
    EXPECT_EQ("https://host/path/file.parquet",
        IceDiskUtils::joinPath("https://host/path", "file.parquet"));
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

TEST(IceDiskUtils_ConstructNodeTablePath, S3URI) {
    EXPECT_EQ("s3://bucket/data/nodes_user.parquet",
        IceDiskUtils::constructNodeTablePath("s3://bucket/data", "user", ".parquet"));
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

TEST(IceDiskUtils_ConstructCSRPaths, S3URI) {
    auto paths = IceDiskUtils::constructCSRPaths("s3://bucket/data", "follows", ".parquet");
    EXPECT_EQ("s3://bucket/data/indices_follows.parquet", paths.indices);
    EXPECT_EQ("s3://bucket/data/indptr_follows.parquet", paths.indptr);
}

// ─────────────────────────────────────────────────────────────
// constructFlatRelTablePath
// ─────────────────────────────────────────────────────────────
TEST(IceDiskUtils_ConstructFlatRelTablePath, EmptyDir) {
    EXPECT_EQ("rels_follows.parquet",
        IceDiskUtils::constructFlatRelTablePath("", "follows", ".parquet"));
}

TEST(IceDiskUtils_ConstructFlatRelTablePath, WithDir) {
    EXPECT_EQ("/some/dir/rels_knows.parquet",
        IceDiskUtils::constructFlatRelTablePath("/some/dir", "knows", ".parquet"));
}

// ─────────────────────────────────────────────────────────────
// checkVersionCompatibility
// ─────────────────────────────────────────────────────────────
class IceDiskCheckVersionTest : public EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
        context = conn->getClientContext();
    }

    ClientContext* context = nullptr;
};

TEST_F(IceDiskCheckVersionTest, NullContext) {
    EXPECT_THROW(
        IceDiskUtils::checkVersionCompatibility(nullptr, DATASET_DIR + "/nodes_user.parquet"),
        RuntimeException);
}

TEST_F(IceDiskCheckVersionTest, FileDoesNotExist) {
    EXPECT_THROW(IceDiskUtils::checkVersionCompatibility(context,
                     DATASET_DIR + "/nodes_nonexistent.parquet"),
        IOException);
}

TEST_F(IceDiskCheckVersionTest, NotAParquetFile) {
    EXPECT_THROW(
        IceDiskUtils::checkVersionCompatibility(context, DATASET_DIR + "/nodes_notparquet.parquet"),
        CopyException);
}

TEST_F(IceDiskCheckVersionTest, MissingVersionKey) {
    EXPECT_NO_THROW(
        IceDiskUtils::checkVersionCompatibility(context, DATASET_DIR + "/nodes_noversion.parquet"));
}

TEST_F(IceDiskCheckVersionTest, WrongVersionValue) {
    try {
        IceDiskUtils::checkVersionCompatibility(context,
            DATASET_DIR + "/nodes_wrongversion.parquet");
        FAIL() << "Expected RuntimeException for wrong version";
    } catch (const RuntimeException& e) {
        EXPECT_TRUE(std::string(e.what()).find("does not support icebug_disk_version: v99") !=
                    std::string::npos);
    }
}

TEST_F(IceDiskCheckVersionTest, UppercaseVersionSucceeds) {
    // "V1" should match "v1" case-insensitively
    EXPECT_NO_THROW(IceDiskUtils::checkVersionCompatibility(context,
        DATASET_DIR + "/nodes_upperversion.parquet"));
}

TEST_F(IceDiskCheckVersionTest, ValidV1Succeeds) {
    EXPECT_NO_THROW(
        IceDiskUtils::checkVersionCompatibility(context, DATASET_DIR + "/nodes_user.parquet"));
}

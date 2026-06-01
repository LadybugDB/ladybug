#include <filesystem>
#include <memory>
#include <string>

#include "gtest/gtest.h"
#include "main/connection.h"
#include "main/database.h"
#include "test_helper/test_helper.h"
#include <format>

using namespace lbug::main;
namespace fs = std::filesystem;

namespace lbug {
namespace testing {

// Verifies that creating a Connection to an on-disk database automatically adds
// the database directory to the VFS file search path.
TEST(FileSearchPathTest, DBInitAddsDBDirToSearchPath) {
    auto dbPath = TestHelper::getTempDBPathStr("FileSearchPath.DBInit");
    auto database = std::make_unique<Database>(dbPath, SystemConfig());
    auto conn = std::make_unique<Connection>(database.get());

    auto expectedDir = fs::path(dbPath).parent_path().lexically_normal().string();
    const auto& searchPath = conn->getClientContext()->getClientConfigUnsafe()->fileSearchPath;
    EXPECT_EQ(searchPath, expectedDir);
}

// Verifies that an in-memory database does not add any entry to the file search path.
TEST(FileSearchPathTest, InMemoryDBHasEmptySearchPath) {
    auto database = std::make_unique<Database>(":memory:", SystemConfig());
    auto conn = std::make_unique<Connection>(database.get());

    const auto& searchPath = conn->getClientContext()->getClientConfigUnsafe()->fileSearchPath;
    EXPECT_EQ(searchPath, "");
}

// Verifies that each new Connection independently receives the db directory in its search path.
TEST(FileSearchPathTest, NewConnectionGetsDBDirSearchPath) {
    auto dbPath = TestHelper::getTempDBPathStr("FileSearchPath.MultiConn");
    auto database = std::make_unique<Database>(dbPath, SystemConfig());
    auto conn1 = std::make_unique<Connection>(database.get());
    auto conn2 = std::make_unique<Connection>(database.get());

    auto expectedDir = fs::path(dbPath).parent_path().lexically_normal().string();
    EXPECT_EQ(conn1->getClientContext()->getClientConfigUnsafe()->fileSearchPath, expectedDir);
    EXPECT_EQ(conn2->getClientContext()->getClientConfigUnsafe()->fileSearchPath, expectedDir);
}

// Verifies that ATTACHing a Lbug database adds the attached database's directory
// to the executing connection's file search path.
TEST(FileSearchPathTest, AttachAddsAttachedDBDirToSearchPath) {
    auto srcDbPath = TestHelper::getTempDBPathStr("FileSearchPath.AttachSrc");
    auto dstDbPath = TestHelper::getTempDBPathStr("FileSearchPath.AttachDst");

    // Create source database on disk
    { auto srcDb = std::make_unique<Database>(srcDbPath, SystemConfig()); }

    auto dstDb = std::make_unique<Database>(dstDbPath, SystemConfig());
    auto conn = std::make_unique<Connection>(dstDb.get());

    auto result = conn->query(std::format("ATTACH '{}' AS srcdb (DBTYPE lbug)", srcDbPath));
    ASSERT_TRUE(result->isSuccess()) << result->toString();

    const auto& searchPath = conn->getClientContext()->getClientConfigUnsafe()->fileSearchPath;
    auto expectedSrcDir = fs::path(srcDbPath).parent_path().lexically_normal().string();
    auto expectedDstDir = fs::path(dstDbPath).parent_path().lexically_normal().string();

    EXPECT_NE(searchPath.find(expectedSrcDir), std::string::npos)
        << "Attached db directory should be in fileSearchPath. Got: " << searchPath;
    EXPECT_NE(searchPath.find(expectedDstDir), std::string::npos)
        << "Main db directory should still be in fileSearchPath. Got: " << searchPath;
}

// Verifies that the attached db directory is prepended (highest priority) in the search path.
TEST(FileSearchPathTest, AttachPrependsSearchPath) {
    auto srcDbPath = TestHelper::getTempDBPathStr("FileSearchPath.Prepend");
    auto dstDbPath = TestHelper::getTempDBPathStr("FileSearchPath.PrependDst");

    { auto srcDb = std::make_unique<Database>(srcDbPath, SystemConfig()); }

    auto dstDb = std::make_unique<Database>(dstDbPath, SystemConfig());
    auto conn = std::make_unique<Connection>(dstDb.get());

    auto result = conn->query(std::format("ATTACH '{}' AS srcdb (DBTYPE lbug)", srcDbPath));
    ASSERT_TRUE(result->isSuccess()) << result->toString();

    const auto& searchPath = conn->getClientContext()->getClientConfigUnsafe()->fileSearchPath;
    auto expectedSrcDir = fs::path(srcDbPath).parent_path().lexically_normal().string();

    // Attached db directory should appear before (or at the start of) the search path
    EXPECT_EQ(searchPath.substr(0, expectedSrcDir.size()), expectedSrcDir)
        << "Attached db directory should be prepended. Got: " << searchPath;
}

// ── Windows-path tests ────────────────────────────────────────────────────────
// These tests call addDBDirToFileSearchPath() directly with Windows-style paths
// (backslash separators) to verify the separator-agnostic logic. They run on
// all platforms because the extraction logic is purely string-based before the
// std::filesystem::absolute() call.

// A Windows absolute path with backslashes should extract the parent directory.
TEST(FileSearchPathTest, WindowsAbsolutePathExtractsDir) {
    auto database = std::make_unique<Database>(":memory:", SystemConfig());
    auto conn = std::make_unique<Connection>(database.get());
    auto* ctx = conn->getClientContext();
    ctx->getClientConfigUnsafe()->fileSearchPath = "";

    ctx->addDBDirToFileSearchPath("C:\\Users\\foo\\bar.lbug");

    const auto& searchPath = ctx->getClientConfigUnsafe()->fileSearchPath;
    // The backslash separator must be recognised: the call must not be a no-op.
    EXPECT_FALSE(searchPath.empty()) << "Windows-style path should add a directory";
    // The filename itself must not appear in the directory entry.
    EXPECT_EQ(searchPath.find("bar.lbug"), std::string::npos)
        << "Filename should be stripped from the directory entry. Got: " << searchPath;
}

// A path with only a backslash separator (root-level) should produce a non-empty entry.
TEST(FileSearchPathTest, WindowsRootBackslashPath) {
    auto database = std::make_unique<Database>(":memory:", SystemConfig());
    auto conn = std::make_unique<Connection>(database.get());
    auto* ctx = conn->getClientContext();
    ctx->getClientConfigUnsafe()->fileSearchPath = "";

    ctx->addDBDirToFileSearchPath("\\file.lbug");

    // slashPos == 0, dirPath becomes "\\" (root), so searchPath must be non-empty
    const auto& searchPath = ctx->getClientConfigUnsafe()->fileSearchPath;
    EXPECT_FALSE(searchPath.empty()) << "Root backslash path should add an entry";
}

// A path with no separator at all (bare filename) must be a no-op on any platform.
TEST(FileSearchPathTest, BareFilenameIsNoOp) {
    auto database = std::make_unique<Database>(":memory:", SystemConfig());
    auto conn = std::make_unique<Connection>(database.get());
    auto* ctx = conn->getClientContext();
    ctx->getClientConfigUnsafe()->fileSearchPath = "";

    ctx->addDBDirToFileSearchPath("bare.lbug");

    EXPECT_EQ(ctx->getClientConfigUnsafe()->fileSearchPath, "")
        << "Bare filename (no separator) should not modify fileSearchPath";
}

// Duplicate Windows-style paths should not be added twice.
TEST(FileSearchPathTest, WindowsPathNotAddedTwice) {
    auto database = std::make_unique<Database>(":memory:", SystemConfig());
    auto conn = std::make_unique<Connection>(database.get());
    auto* ctx = conn->getClientContext();
    ctx->getClientConfigUnsafe()->fileSearchPath = "";

    ctx->addDBDirToFileSearchPath("C:\\Users\\foo\\bar.lbug");
    const auto firstPath = ctx->getClientConfigUnsafe()->fileSearchPath;

    ctx->addDBDirToFileSearchPath("C:\\Users\\foo\\other.lbug");
    const auto secondPath = ctx->getClientConfigUnsafe()->fileSearchPath;

    EXPECT_EQ(firstPath, secondPath) << "Same directory should not be added twice";
}

} // namespace testing
} // namespace lbug

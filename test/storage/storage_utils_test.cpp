#include <filesystem>
#include <string>

#include "gtest/gtest.h"
#include "storage/storage_utils.h"

#ifdef _WIN32
namespace {

class CurrentPathGuard {
public:
    CurrentPathGuard() : path{std::filesystem::current_path()} {}

    ~CurrentPathGuard() {
        std::error_code ec;
        std::filesystem::current_path(path, ec);
    }

private:
    std::filesystem::path path;
};

} // namespace
#endif

using namespace lbug::storage;

TEST(StorageUtilsTest, WindowsDrivePathDoesNotNeedCurrentDirectory) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows path semantics regression test.";
#else
    CurrentPathGuard guard;
    const auto deletedCwd =
        std::filesystem::temp_directory_path() / "lbug_storage_utils_deleted_cwd";
    std::filesystem::remove_all(deletedCwd);
    std::filesystem::create_directories(deletedCwd);
    std::filesystem::current_path(deletedCwd);
    std::error_code removeError;
    std::filesystem::remove_all(deletedCwd, removeError);

    const auto expanded =
        StorageUtils::expandPath(nullptr, R"(C:\adham\lbug-test\..\db_wasm_iso.lbug)");
    EXPECT_EQ(std::filesystem::path{R"(C:\adham\db_wasm_iso.lbug)"}.string(), expanded);

    const auto expandedDrivePath =
        StorageUtils::expandPath(nullptr, R"(C:adham\lbug-test\..\db_wasm_iso.lbug)");
    EXPECT_EQ(std::filesystem::path{R"(C:adham\db_wasm_iso.lbug)"}.string(), expandedDrivePath);
#endif
}

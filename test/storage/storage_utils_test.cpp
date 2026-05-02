#include <filesystem>
#include <string>

#include "gtest/gtest.h"
#include "storage/storage_utils.h"

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
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
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    GTEST_SKIP() << "Windows host path semantics regression test.";
#else
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    CurrentPathGuard guard;
    const auto deletedCwd =
        std::filesystem::temp_directory_path() / "lbug_storage_utils_deleted_cwd";
    std::filesystem::remove_all(deletedCwd);
    std::filesystem::create_directories(deletedCwd);
    std::filesystem::current_path(deletedCwd);
    std::error_code removeError;
    std::filesystem::remove_all(deletedCwd, removeError);
#endif

    const auto expanded =
        StorageUtils::expandPath(nullptr, R"(C:\adham\lbug-test\..\db_wasm_iso.lbug)");
#ifdef __EMSCRIPTEN__
    EXPECT_EQ("C:/adham/db_wasm_iso.lbug", expanded);
#else
    EXPECT_EQ(std::filesystem::path{R"(C:\adham\db_wasm_iso.lbug)"}.string(), expanded);
#endif

    const auto expandedDrivePath =
        StorageUtils::expandPath(nullptr, R"(C:adham\lbug-test\..\db_wasm_iso.lbug)");
#ifdef __EMSCRIPTEN__
    EXPECT_EQ("C:adham/db_wasm_iso.lbug", expandedDrivePath);
#else
    EXPECT_EQ(std::filesystem::path{R"(C:adham\db_wasm_iso.lbug)"}.string(), expandedDrivePath);
#endif
#endif
}

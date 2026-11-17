#pragma once

#include <filesystem>
#include <string>

namespace lbug {
namespace common {

// File and database paths arrive as UTF-8. std::filesystem::path built from a
// narrow string decodes per process code page on Windows, and string() /
// generic_string() encode per process code page. Use these helpers for every
// narrow <-> path conversion so non-ASCII paths (including Unicode working
// directories) keep working regardless of the process code page.
//
// On POSIX the conversions are byte-preserving pass-throughs.
struct PathUtils {
    static inline std::filesystem::path toNativePath(const std::string& utf8Path) {
#if defined(_WIN32)
        return std::filesystem::path(std::u8string(utf8Path.begin(), utf8Path.end()));
#else
        return std::filesystem::path(utf8Path);
#endif
    }

    static inline std::string toGenericUTF8String(const std::filesystem::path& path) {
#if defined(_WIN32)
        const auto u8str = path.generic_u8string();
        return std::string(u8str.begin(), u8str.end());
#else
        return path.generic_string();
#endif
    }

    // absolute() + lexically_normal() round-trip returning generic-separator
    // UTF-8, for file-search-path entries. Using the generic form (instead of
    // native .u8string()) keeps entries comparable with expandPath() output.
    static inline std::string absoluteLexicallyNormalUTF8(const std::string& utf8Path) {
        return toGenericUTF8String(
            std::filesystem::absolute(toNativePath(utf8Path)).lexically_normal());
    }
};

} // namespace common
} // namespace lbug

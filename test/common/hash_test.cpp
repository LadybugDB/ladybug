#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include "function/hash/hash_functions.h"
#include "gtest/gtest.h"

// Regression test for the UBSAN misaligned uint64_t load in
// Hash::operation<std::string_view> (issue #879). Ladybug's inline
// short-string storage can hand out character data that is only 4-byte
// aligned, so the hash must not dereference it as uint64_t*.
TEST(HashTest, UnalignedStringViewHashMatchesAligned) {
    constexpr std::string_view bytes = "Elizabeth0123456789"; // 19 bytes: multi-block + tail
    static_assert(bytes.size() >= 8u);
    alignas(uint64_t) char alignedBacking[32];
    alignas(uint64_t) char unalignedBacking[32];
    ASSERT_LT(bytes.size() + 1, sizeof(unalignedBacking));
    std::memcpy(alignedBacking, bytes.data(), bytes.size());
    std::memcpy(unalignedBacking + 1, bytes.data(), bytes.size());
    std::string_view aligned(alignedBacking, bytes.size());
    std::string_view unaligned(unalignedBacking + 1, bytes.size());
    ASSERT_EQ(reinterpret_cast<uintptr_t>(aligned.data()) % alignof(uint64_t), 0u);
    ASSERT_NE(reinterpret_cast<uintptr_t>(unaligned.data()) % alignof(uint64_t), 0u);
    lbug::common::hash_t alignedHash = 0;
    lbug::common::hash_t unalignedHash = 0;
    lbug::function::Hash::operation(aligned, alignedHash);
    lbug::function::Hash::operation(unaligned, unalignedHash);
    EXPECT_EQ(unalignedHash, alignedHash);
}

// Regression test for issue #1092: the char-signedness portability fix (issue #882, v0.21.0)
// changed string hash values for keys with bytes >= 0x80 on signed-char platforms, so primary
// key hash indexes (including FTS internal term tables) persisted by <= 0.20.x misplace
// non-ASCII keys when opened by >= 0.21.0. Pin the portable values below so any future change
// to the string hash function fails loudly here instead of silently corrupting persisted
// indexes. Keys use explicit byte escapes so the test source itself is exec-charset
// independent. Expected values were produced by Hash::operation on the portable
// (zero-extending) implementation.
TEST(HashTest, NonAsciiStringHashIsPortable) {
    const std::pair<std::string_view, lbug::common::hash_t> cases[] = {
        {"\xE2\x86\x92", 9317534602650280856ULL},              // U+2192 RIGHTWARDS ARROW
        {"z\xC3\xBCrich", 5377299857227594317ULL},             // zürich
        {"Z\xC3\xBCrich", 4246789619063248512ULL},             // Zürich
        {"Caf\xC3\xA9", 14810678864071362944ULL},              // Café
        {"\xE6\x9D\xB1\xE4\xBA\xAC", 11346535282121821487ULL}, // U+6771 U+4EAC
        {"\xF0\x9F\x98\x80", 12263534235179796242ULL},         // U+1F600 emoji
        {"plain", 3582540791184550914ULL},                     // ASCII control
        {"allen-p", 8924945747639595936ULL},                   // ASCII control
        {"102", 13476785377723977377ULL},                      // ASCII control
    };
    for (const auto& [key, expected] : cases) {
        lbug::common::hash_t actual = 0;
        lbug::function::Hash::operation(key, actual);
        EXPECT_EQ(actual, expected) << "non-portable hash for " << key.size() << "-byte key";
    }
}

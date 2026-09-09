#include <cstdint>
#include <cstring>
#include <string_view>

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

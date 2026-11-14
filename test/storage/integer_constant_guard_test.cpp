#include <cstdint>
#include <cstring>
#include <vector>

#include "common/types/types.h"
#include "gtest/gtest.h"
#include "storage/compression/compression.h"
#include "storage/table/column_chunk_metadata.h"

using namespace lbug::common;
using namespace lbug::storage;

// Regression tests for gh-1447: MSVC's vectorized std::minmax_element can return
// min == max for non-constant uint8_t/int8_t data, which previously selected CONSTANT
// compression and silently dropped data at checkpoint. Two layers of defense:
//  1. getTypedMinMax uses a plain loop (no vectorized STL path).
//  2. GetCompressionMetadata verifies bitwise equality before choosing CONSTANT, so any
//     residual min/max error only costs compression, never data.

template<typename T>
std::span<const uint8_t> toBytes(const std::vector<T>& buffer) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(buffer.data()),
        buffer.size() * sizeof(T));
}

template<typename T>
ColumnChunkMetadata decideWithForcedMinMax(const std::vector<T>& buffer, LogicalType dataType,
    StorageValue forcedMin, StorageValue forcedMax) {
    auto alg = std::make_shared<IntegerBitpacking<T>>();
    return GetCompressionMetadata(alg, dataType)(toBytes(buffer), buffer.size(), forcedMin,
        forcedMax);
}

template<typename T>
ColumnChunkMetadata decideViaMinMax(const std::vector<T>& buffer, LogicalType dataType) {
    auto alg = std::make_shared<IntegerBitpacking<T>>();
    auto [min, max] = getMinMaxStorageValue(reinterpret_cast<const uint8_t*>(buffer.data()), 0,
        buffer.size(), dataType.getPhysicalType(), nullptr, true);
    EXPECT_TRUE(min.has_value());
    EXPECT_TRUE(max.has_value());
    return GetCompressionMetadata(alg, dataType)(toBytes(buffer), buffer.size(),
        min.value_or(StorageValue{}), max.value_or(StorageValue{}));
}

// The exact byte pattern from the issue: "ab" * 8192 plus "second".
static std::vector<uint8_t> issuePatternU8() {
    std::vector<uint8_t> v;
    v.reserve(8192 * 2 + 6);
    for (int i = 0; i < 8192; ++i) {
        v.push_back(static_cast<uint8_t>('a'));
        v.push_back(static_cast<uint8_t>('b'));
    }
    for (char c : {'s', 'e', 'c', 'o', 'n', 'd'}) {
        v.push_back(static_cast<uint8_t>(c));
    }
    return v;
}

static std::vector<int8_t> issuePatternI8() {
    std::vector<int8_t> v;
    v.reserve(8192 * 2 + 6);
    for (int i = 0; i < 8192; ++i) {
        v.push_back(static_cast<int8_t>('a'));
        v.push_back(static_cast<int8_t>('b'));
    }
    for (char c : {'s', 'e', 'c', 'o', 'n', 'd'}) {
        v.push_back(static_cast<int8_t>(c));
    }
    return v;
}

TEST(IntegerConstantGuardTests, Uint8IssuePatternMinMaxIsCorrect) {
    auto buffer = issuePatternU8();
    auto [min, max] = getMinMaxStorageValue(buffer.data(), 0, buffer.size(), PhysicalTypeID::UINT8,
        nullptr, true);
    ASSERT_TRUE(min.has_value());
    ASSERT_TRUE(max.has_value());
    EXPECT_EQ(min->get<uint8_t>(), 97);
    EXPECT_EQ(max->get<uint8_t>(), 115);
}

TEST(IntegerConstantGuardTests, Int8IssuePatternMinMaxIsCorrect) {
    auto buffer = issuePatternI8();
    auto [min, max] = getMinMaxStorageValue(reinterpret_cast<const uint8_t*>(buffer.data()), 0,
        buffer.size(), PhysicalTypeID::INT8, nullptr, true);
    ASSERT_TRUE(min.has_value());
    ASSERT_TRUE(max.has_value());
    EXPECT_EQ(min->get<int8_t>(), 97);
    EXPECT_EQ(max->get<int8_t>(), 115);
}

TEST(IntegerConstantGuardTests, Uint8IssuePatternIsNotConstant) {
    auto buffer = issuePatternU8();
    const auto metadata = decideViaMinMax(buffer, LogicalType::UINT8());
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(IntegerConstantGuardTests, Int8IssuePatternIsNotConstant) {
    auto buffer = issuePatternI8();
    const auto metadata = decideViaMinMax(buffer, LogicalType::INT8());
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(IntegerConstantGuardTests, WrongMinMaxCannotForceConstantUint8) {
    // Simulates the MSVC bug: min/max fold reports min == max on non-constant data.
    // The bitwise guard must prevent CONSTANT.
    std::vector<uint8_t> buffer{97, 98, 97, 98, 115};
    const auto metadata = decideWithForcedMinMax<uint8_t>(buffer, LogicalType::UINT8(),
        StorageValue(static_cast<uint8_t>(97)), StorageValue(static_cast<uint8_t>(97)));
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(IntegerConstantGuardTests, WrongMinMaxCannotForceConstantInt8) {
    std::vector<int8_t> buffer{97, 98, 97, 98, 115};
    const auto metadata = decideWithForcedMinMax<int8_t>(buffer, LogicalType::INT8(),
        StorageValue(static_cast<int8_t>(97)), StorageValue(static_cast<int8_t>(97)));
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(IntegerConstantGuardTests, WrongMinMaxCannotForceConstantUint32) {
    std::vector<uint32_t> buffer{1, 2, 1, 2, 7};
    const auto metadata = decideWithForcedMinMax<uint32_t>(buffer, LogicalType::UINT32(),
        StorageValue(static_cast<uint32_t>(1)), StorageValue(static_cast<uint32_t>(1)));
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(IntegerConstantGuardTests, WrongMinMaxCannotForceConstantInt64) {
    std::vector<int64_t> buffer{5, 6, 5, 6, 9};
    const auto metadata = decideWithForcedMinMax<int64_t>(buffer, LogicalType::INT64(),
        StorageValue(static_cast<int64_t>(5)), StorageValue(static_cast<int64_t>(5)));
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(IntegerConstantGuardTests, TrueConstantStillDetectedUint8) {
    // Guard against overcorrection: uniform chunks must still use CONSTANT.
    std::vector<uint8_t> buffer(64, 42);
    const auto metadata = decideViaMinMax(buffer, LogicalType::UINT8());
    EXPECT_EQ(metadata.compMeta.compression, CompressionType::CONSTANT);
    EXPECT_EQ(metadata.compMeta.min.get<uint8_t>(), 42);
}

TEST(IntegerConstantGuardTests, TrueConstantStillDetectedInt32) {
    std::vector<int32_t> buffer(64, -7);
    const auto metadata = decideViaMinMax(buffer, LogicalType::INT32());
    EXPECT_EQ(metadata.compMeta.compression, CompressionType::CONSTANT);
    EXPECT_EQ(metadata.compMeta.min.get<int32_t>(), -7);
}

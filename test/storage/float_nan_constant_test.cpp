#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "common/null_mask.h"
#include "common/types/types.h"
#include "graph_test/private_graph_test.h"
#include "gtest/gtest.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/compression/compression.h"
#include "storage/compression/float_compression.h"
#include "storage/storage_manager.h"
#include "storage/table/column_chunk_data.h"
#include "storage/table/column_chunk_metadata.h"
#include "storage/table/column_reader_writer.h"
#include "storage/table/compression_flush_buffer.h"
#include <concepts>

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace testing {

// Regression tests for gh-1043: a NaN (or -0.0) in a DOUBLE/FLOAT chunk must not
// collapse the whole chunk to CONSTANT on flush. The flush-time min/max fold is
// NaN-unaware, so min == max can hold while the chunk holds distinct values;
// the CONSTANT decision therefore requires a bitwise all-equal check.

template<std::floating_point T>
LogicalType floatLogicalType() {
    if constexpr (std::same_as<T, double>) {
        return LogicalType::DOUBLE();
    } else {
        return LogicalType::FLOAT();
    }
}

template<std::floating_point T>
std::span<const uint8_t> toBytes(const std::vector<T>& buffer) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(buffer.data()),
        buffer.size() * sizeof(T));
}

// Exercises the production flush-decision path: getMinMaxStorageValue followed by
// the top-level GetCompressionMetadata dispatcher (the functor ColumnChunkData
// uses in getMetadataToFlush).
template<std::floating_point T>
ColumnChunkMetadata decideMetadata(const std::vector<T>& buffer, const NullMask* nullMask) {
    const auto dataType = floatLogicalType<T>();
    const auto alg = std::make_shared<FloatCompression<T>>();
    auto [min, max] = getMinMaxStorageValue(reinterpret_cast<const uint8_t*>(buffer.data()), 0,
        buffer.size(), dataType.getPhysicalType(), nullMask, true);
    return GetCompressionMetadata(alg, dataType)(toBytes(buffer), buffer.size(),
        min.value_or(StorageValue{}), max.value_or(StorageValue{}));
}

template<std::floating_point T>
bool bitwiseEqual(const std::vector<T>& a, const std::vector<T>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0;
}

template<std::floating_point T>
T nanWithPayload(uint64_t payload) {
    static_assert(sizeof(T) == 4 || sizeof(T) == 8);
    uint64_t bits;
    if constexpr (sizeof(T) == 8) {
        bits = 0x7FF8000000000000ULL | (payload & 0x000FFFFFFFFFFFFFULL);
    } else {
        bits = 0x7FC00000ULL | (payload & 0x003FFFFFULL);
    }
    T value{};
    std::memcpy(&value, &bits, sizeof(T));
    DASSERT(std::isnan(value));
    return value;
}

TEST(FloatNaNConstantMetadataTests, DoubleNaNFirstIsNotConstant) {
    std::vector<double> buffer{std::numeric_limits<double>::quiet_NaN(), 1.5, 2.5, 3.5, 4.5};
    const auto metadata = decideMetadata(buffer, nullptr);
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(FloatNaNConstantMetadataTests, FloatNaNFirstIsNotConstant) {
    std::vector<float> buffer{std::numeric_limits<float>::quiet_NaN(), 1.5f, 2.5f, 3.5f, 4.5f};
    const auto metadata = decideMetadata(buffer, nullptr);
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(FloatNaNConstantMetadataTests, DoubleNaNLaterAmongEqualValuesIsNotConstant) {
    std::vector<double> buffer{5.0, 5.0, std::numeric_limits<double>::quiet_NaN(), 5.0, 5.0};
    const auto metadata = decideMetadata(buffer, nullptr);
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(FloatNaNConstantMetadataTests, DoubleNaNLastIsNotConstant) {
    std::vector<double> buffer{1.5, 2.5, 3.5, 4.5, std::numeric_limits<double>::quiet_NaN()};
    const auto metadata = decideMetadata(buffer, nullptr);
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(FloatNaNConstantMetadataTests, DoubleMixedZerosAreNotConstant) {
    // -0.0 compares equal to 0.0 but differs bitwise; the sign must survive flush.
    std::vector<double> buffer{0.0, -0.0, 0.0};
    const auto metadata = decideMetadata(buffer, nullptr);
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(FloatNaNConstantMetadataTests, FloatMixedZerosAreNotConstant) {
    std::vector<float> buffer{0.0f, -0.0f, 0.0f};
    const auto metadata = decideMetadata(buffer, nullptr);
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(FloatNaNConstantMetadataTests, DoubleDistinctNaNPayloadsAreNotConstant) {
    std::vector<double> buffer{nanWithPayload<double>(1), nanWithPayload<double>(2)};
    const auto metadata = decideMetadata(buffer, nullptr);
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(FloatNaNConstantMetadataTests, DoubleTrueConstantStillDetected) {
    // Guard against overcorrection: uniform chunks must still use CONSTANT.
    std::vector<double> buffer(16, 5.0);
    const auto metadata = decideMetadata(buffer, nullptr);
    EXPECT_EQ(metadata.compMeta.compression, CompressionType::CONSTANT);
    EXPECT_EQ(metadata.compMeta.min.get<double>(), 5.0);
}

TEST(FloatNaNConstantMetadataTests, DoubleAllSameNaNMayStayConstant) {
    // Bitwise-identical NaNs genuinely are one value; CONSTANT is correct here.
    std::vector<double> buffer(16, std::numeric_limits<double>::quiet_NaN());
    const auto metadata = decideMetadata(buffer, nullptr);
    EXPECT_EQ(metadata.compMeta.compression, CompressionType::CONSTANT);
    EXPECT_TRUE(std::isnan(metadata.compMeta.min.get<double>()));
}

TEST(FloatNaNConstantMetadataTests, DoubleNaNFirstBehindNullsIsNotConstant) {
    // Leading NULLs must not change anything: the first non-null value is NaN.
    std::vector<double> buffer{0.0, 0.0, std::numeric_limits<double>::quiet_NaN(), 1.5, 2.5};
    NullMask nullMask(buffer.size());
    nullMask.setNull(0, true);
    nullMask.setNull(1, true);
    const auto metadata = decideMetadata(buffer, &nullMask);
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(FloatNaNConstantMetadataTests, DoubleMinMaxSkipsNaN) {
    // Zone-map statistics must reflect the comparable values, not a leading NaN.
    std::vector<double> buffer{std::numeric_limits<double>::quiet_NaN(), 1.5, 2.5, 4.5, 3.5};
    auto [min, max] = getMinMaxStorageValue(reinterpret_cast<const uint8_t*>(buffer.data()), 0,
        buffer.size(), PhysicalTypeID::DOUBLE, nullptr, true);
    ASSERT_TRUE(min.has_value());
    ASSERT_TRUE(max.has_value());
    EXPECT_EQ(min->get<double>(), 1.5);
    EXPECT_EQ(max->get<double>(), 4.5);
}

TEST(FloatNaNConstantMetadataTests, DoubleMinMaxSkipsNaNWithNulls) {
    std::vector<double> buffer{0.0, std::numeric_limits<double>::quiet_NaN(), 1.5, 4.5};
    NullMask nullMask(buffer.size());
    nullMask.setNull(0, true);
    auto [min, max] = getMinMaxStorageValue(reinterpret_cast<const uint8_t*>(buffer.data()), 0,
        buffer.size(), PhysicalTypeID::DOUBLE, &nullMask, true);
    ASSERT_TRUE(min.has_value());
    ASSERT_TRUE(max.has_value());
    EXPECT_EQ(min->get<double>(), 1.5);
    EXPECT_EQ(max->get<double>(), 4.5);
}

TEST(FloatNaNConstantMetadataTests, DoubleAllNaNMinMaxFallsBackToFirst) {
    std::vector<double> buffer(8, std::numeric_limits<double>::quiet_NaN());
    auto [min, max] = getMinMaxStorageValue(reinterpret_cast<const uint8_t*>(buffer.data()), 0,
        buffer.size(), PhysicalTypeID::DOUBLE, nullptr, true);
    ASSERT_TRUE(min.has_value());
    ASSERT_TRUE(max.has_value());
    EXPECT_TRUE(std::isnan(min->get<double>()));
    EXPECT_TRUE(std::isnan(max->get<double>()));
}

class FloatNaNConstantFlushTest : public DBTest {
public:
    void SetUp() override {
        BaseGraphTest::SetUp(); // NOLINT
        createDBAndConn();
    }

    std::string getInputDir() override {
        return TestHelper::appendLbugRootPath("dataset/tinysnb/");
    }

    template<std::floating_point T>
    void checkFlushRoundTrip(const std::vector<T>& buffer);
};

template<std::floating_point T>
void FloatNaNConstantFlushTest::checkFlushRoundTrip(const std::vector<T>& buffer) {
    auto* mm = getMemoryManager(*database);
    auto* storageManager = getStorageManager(*database);
    auto* dataFH = storageManager->getDataFH();
    const auto dataType = floatLogicalType<T>();
    const auto alg = std::make_shared<FloatCompression<T>>();

    const auto preScanMetadata = decideMetadata(buffer, nullptr);
    EXPECT_NE(preScanMetadata.compMeta.compression, CompressionType::CONSTANT);
    if (preScanMetadata.compMeta.compression == CompressionType::CONSTANT) {
        return;
    }
    auto allocatedBlock =
        dataFH->getPageManager()->allocatePageRange(preScanMetadata.getNumPages());
    const auto flushedMetadata = CompressedFloatFlushBuffer<T>{alg, dataType}(toBytes(buffer),
        dataFH, allocatedBlock, preScanMetadata);

    auto columnReader = ColumnReadWriterFactory::createColumnReadWriter(dataType.getPhysicalType(),
        dataFH, &storageManager->getShadowFile());
    conn->query("BEGIN TRANSACTION;");
    SegmentState state;
    state.metadata = flushedMetadata;
    state.numValuesPerPage = flushedMetadata.compMeta.numValues(LBUG_PAGE_SIZE, dataType);
    if (flushedMetadata.compMeta.compression == CompressionType::ALP) {
        state.alpExceptionChunk = std::make_unique<InMemoryExceptionChunk<T>>(state, dataFH, mm,
            &storageManager->getShadowFile());
    }
    std::vector<T> out(buffer.size());
    columnReader->readCompressedValuesToPage(state, reinterpret_cast<uint8_t*>(out.data()), 0, 0,
        out.size(), ReadCompressedValuesFromPage(dataType));
    // Bitwise comparison: distinguishes NaN payloads and -0.0 from 0.0.
    EXPECT_TRUE(bitwiseEqual(buffer, out));
}

TEST_F(FloatNaNConstantFlushTest, DoubleNaNFirstRoundTrip) {
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> buffer(256);
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = 1.5 + static_cast<double>(i) * 0.25;
    }
    buffer[0] = nan;
    checkFlushRoundTrip(buffer);
}

TEST_F(FloatNaNConstantFlushTest, DoubleNaNLaterAmongEqualValuesRoundTrip) {
    std::vector<double> buffer(256, 5.0);
    buffer[100] = std::numeric_limits<double>::quiet_NaN();
    checkFlushRoundTrip(buffer);
}

TEST_F(FloatNaNConstantFlushTest, DoubleMixedZerosRoundTrip) {
    std::vector<double> buffer(256, 0.0);
    buffer[10] = -0.0;
    buffer[200] = -0.0;
    checkFlushRoundTrip(buffer);
}

TEST_F(FloatNaNConstantFlushTest, FloatNaNFirstRoundTrip) {
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> buffer(256);
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = 1.5f + static_cast<float>(i) * 0.25f;
    }
    buffer[0] = nan;
    checkFlushRoundTrip(buffer);
}

} // namespace testing
} // namespace lbug

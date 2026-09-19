#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "alp/encode.hpp"
#include "common/exception/storage.h"
#include "common/system_config.h"
#include "gtest/gtest.h"
#include "storage/compression/float_compression.h"
#include "storage/table/column_chunk_data.h"
#include "storage/table/column_chunk_metadata.h"
#include <span>

using namespace lbug::common;
using namespace lbug::storage;

namespace {

template<typename T>
std::span<const uint8_t> asByteSpan(const std::vector<T>& values) {
    return {reinterpret_cast<const uint8_t*>(values.data()), values.size() * sizeof(T)};
}

std::vector<double> getExceptionalValues() {
    std::vector<double> values(256, 5.6);
    values[0] = 0;
    values[2] = 54387589437957.834;
    for (size_t i = 102; i < values.size(); i += 100) {
        values[i] = values[i - 100] + 4385498.234;
    }
    return values;
}

ColumnChunkMetadata getALPMetadata(const std::vector<double>& values) {
    auto alg = std::make_shared<FloatCompression<double>>();
    const auto [min, max] = std::minmax_element(values.begin(), values.end());
    return GetFloatCompressionMetadata<double>{alg, LogicalType::DOUBLE()}(asByteSpan(values),
        values.size(), StorageValue{*min}, StorageValue{*max});
}

} // namespace

TEST(ALPExceptionChunkTest, RejectsMissingOrUnexpectedExceptionChunk) {
    SegmentState state;

    EXPECT_THROW(state.getExceptionChunk<double>(), StorageException);
    EXPECT_THROW(state.getExceptionChunkConst<float>(), StorageException);

    state.alpExceptionChunk = std::unique_ptr<InMemoryExceptionChunk<float>>{};
    EXPECT_THROW(state.getExceptionChunk<double>(), StorageException);
    EXPECT_THROW(state.getExceptionChunkConst<float>(), StorageException);
}

TEST(ALPExceptionChunkTest, RejectsInvalidCompressionMetadata) {
    const CompressionMetadata metadata{StorageValue{0.0}, StorageValue{1.0},
        CompressionType::UNCOMPRESSED};
    FloatCompression<double> alg;
    const uint8_t* source = nullptr;
    std::array<uint8_t, LBUG_PAGE_SIZE> destination{};
    std::array<std::byte, EncodeException<double>::sizeInBytes()> exceptionBuffer{};
    uint64_t exceptionCount = 0;

    EXPECT_THROW(alg.compressNextPageWithExceptions(source, 0, 1, destination.data(),
                     destination.size(), EncodeExceptionView<double>{exceptionBuffer.data()},
                     exceptionBuffer.size(), exceptionCount, metadata),
        StorageException);
    EXPECT_THROW(FloatCompression<double>::getNumDataPages(0, metadata), StorageException);
}

TEST(ALPExceptionChunkTest, RejectsExceptionBufferOverflow) {
    const auto values = getExceptionalValues();
    const auto metadata = getALPMetadata(values);
    ASSERT_EQ(metadata.compMeta.compression, CompressionType::ALP);

    FloatCompression<double> alg;
    const uint8_t* source = reinterpret_cast<const uint8_t*>(values.data());
    std::array<uint8_t, LBUG_PAGE_SIZE> destination{};
    std::array<std::byte, EncodeException<double>::sizeInBytes()> exceptionBuffer{};
    uint64_t exceptionCount = 0;

    EXPECT_THROW(alg.compressNextPageWithExceptions(source, 0, values.size(), destination.data(),
                     destination.size(), EncodeExceptionView<double>{exceptionBuffer.data()}, 0,
                     exceptionCount, metadata.compMeta),
        StorageException);
}

TEST(ALPExceptionChunkTest, RejectsExceptionPositionOverflow) {
    const auto values = getExceptionalValues();
    const auto metadata = getALPMetadata(values);
    ASSERT_EQ(metadata.compMeta.compression, CompressionType::ALP);

    FloatCompression<double> alg;
    const uint8_t* source = reinterpret_cast<const uint8_t*>(values.data());
    std::array<uint8_t, LBUG_PAGE_SIZE> destination{};
    std::array<std::byte, EncodeException<double>::sizeInBytes() * 2> exceptionBuffer{};
    uint64_t exceptionCount = 0;

    EXPECT_THROW(alg.compressNextPageWithExceptions(source, std::numeric_limits<uint32_t>::max(),
                     values.size(), destination.data(), destination.size(),
                     EncodeExceptionView<double>{exceptionBuffer.data()}, exceptionBuffer.size(),
                     exceptionCount, metadata.compMeta),
        StorageException);
}

TEST(ALPExceptionChunkTest, RejectsExceptionPagesBeyondColumnPages) {
    const auto values = getExceptionalValues();
    const auto metadata = getALPMetadata(values);
    ASSERT_EQ(metadata.compMeta.compression, CompressionType::ALP);

    EXPECT_THROW(FloatCompression<double>::getNumDataPages(0, metadata.compMeta), StorageException);
}

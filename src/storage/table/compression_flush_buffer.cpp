#include "storage/table/compression_flush_buffer.h"

#include <type_traits>

#include "common/exception/storage.h"
#include "common/types/types.h"
#include "storage/file_handle.h"
#include "storage/page_manager.h"
#include "storage/table/column_chunk_data.h"
#include <concepts>

namespace lbug::storage {
using namespace common;
using namespace transaction;

ColumnChunkMetadata uncompressedFlushBuffer(std::span<const uint8_t> buffer, FileHandle* dataFH,
    const PageRange& entry, const ColumnChunkMetadata& metadata) {
    DASSERT(dataFH->getNumPages() >= entry.startPageIdx + entry.numPages);
    DASSERT(buffer.size_bytes() <= entry.numPages * LBUG_PAGE_SIZE);
    dataFH->writePagesToFile(buffer.data(), buffer.size(), entry.startPageIdx);
    return ColumnChunkMetadata(entry.startPageIdx, entry.numPages, metadata.numValues,
        metadata.compMeta);
}

ColumnChunkMetadata CompressedFlushBuffer::operator()(std::span<const uint8_t> buffer,
    FileHandle* dataFH, const PageRange& entry, const ColumnChunkMetadata& metadata) const {
    auto valuesRemaining = metadata.numValues;
    const uint8_t* bufferStart = buffer.data();
    const auto compressedBuffer = std::make_unique<uint8_t[]>(LBUG_PAGE_SIZE);
    auto numPages = 0u;
    const auto numValuesPerPage = metadata.compMeta.numValues(LBUG_PAGE_SIZE, dataType);
    DASSERT(numValuesPerPage * entry.numPages >= metadata.numValues);
    while (valuesRemaining > 0) {
        const auto compressedSize = alg->compressNextPage(bufferStart, valuesRemaining,
            compressedBuffer.get(), LBUG_PAGE_SIZE, metadata.compMeta);
        // Avoid underflows (when data is compressed to nothing, numValuesPerPage may be
        // UINT64_MAX)
        if (numValuesPerPage > valuesRemaining) {
            valuesRemaining = 0;
        } else {
            valuesRemaining -= numValuesPerPage;
        }
        if (compressedSize < LBUG_PAGE_SIZE) {
            memset(compressedBuffer.get() + compressedSize, 0, LBUG_PAGE_SIZE - compressedSize);
        }
        DASSERT(numPages < entry.numPages);
        DASSERT(dataFH->getNumPages() >= entry.startPageIdx + numPages);
        dataFH->writePageToFile(compressedBuffer.get(), entry.startPageIdx + numPages);
        numPages++;
    }
    // Make sure that the on-disk file is the right length
    if (!dataFH->isInMemoryMode() && numPages < entry.numPages) {
        memset(compressedBuffer.get(), 0, LBUG_PAGE_SIZE);
        while (numPages < entry.numPages) {
            dataFH->writePageToFile(compressedBuffer.get(), entry.startPageIdx + numPages);
            ++numPages;
        }
    }
    return ColumnChunkMetadata(entry.startPageIdx, entry.numPages, metadata.numValues,
        metadata.compMeta);
}

namespace {
template<std::floating_point T>
std::pair<std::unique_ptr<uint8_t[]>, uint64_t> flushCompressedFloats(const CompressionAlg& alg,
    PhysicalTypeID dataType, std::span<const uint8_t> buffer, FileHandle* dataFH,
    const PageRange& entry, const ColumnChunkMetadata& metadata) {
    const auto& castedAlg = dynamic_cast_checked<const FloatCompression<T>&>(alg);

    const auto& floatMetadata = FloatCompression<T>::getValidatedMetadata(metadata.compMeta);
    if (floatMetadata.exceptionCapacity < floatMetadata.exceptionCount) [[unlikely]] {
        throw StorageException("ALP exception count exceeds its declared capacity.");
    }

    auto valuesRemaining = metadata.numValues;
    if (valuesRemaining > buffer.size_bytes() / sizeof(T)) [[unlikely]] {
        throw StorageException("ALP compression input buffer is smaller than its metadata.");
    }

    const size_t exceptionBufferSize =
        EncodeException<T>::numPagesFromExceptions(floatMetadata.exceptionCapacity) *
        LBUG_PAGE_SIZE;
    auto exceptionBuffer = std::make_unique<uint8_t[]>(exceptionBufferSize);
    std::byte* exceptionBufferCursor = reinterpret_cast<std::byte*>(exceptionBuffer.get());

    const auto numValuesPerPage = metadata.compMeta.numValues(LBUG_PAGE_SIZE, dataType);
    const auto numDataPages = metadata.getNumDataPages(dataType);
    const auto requiredDataPages =
        numValuesPerPage == 0          ? UINT64_MAX :
        numValuesPerPage == UINT64_MAX ? (metadata.numValues == 0 ? 0 : 1) :
                                         common::ceilDiv(metadata.numValues, numValuesPerPage);
    if (numValuesPerPage == 0 || requiredDataPages > numDataPages) [[unlikely]] {
        throw StorageException("ALP data page capacity is inconsistent with column metadata.");
    }
    const auto numExceptionPages =
        EncodeException<T>::numPagesFromExceptions(floatMetadata.exceptionCapacity);
    if (numExceptionPages > entry.numPages ||
        requiredDataPages > entry.numPages - numExceptionPages ||
        entry.startPageIdx == INVALID_PAGE_IDX ||
        static_cast<uint64_t>(entry.startPageIdx) + entry.numPages > dataFH->getNumPages())
        [[unlikely]] {
        throw StorageException("ALP column page range is invalid during compression.");
    }

    const auto compressedBuffer = std::make_unique<uint8_t[]>(LBUG_PAGE_SIZE);
    const uint8_t* bufferCursor = buffer.data();
    auto numPages = 0u;
    size_t remainingExceptionBufferSize = exceptionBufferSize;
    size_t totalExceptionCount = 0;

    while (valuesRemaining > 0) {
        uint64_t pageExceptionCount = 0;
        (void)castedAlg.compressNextPageWithExceptions(bufferCursor,
            metadata.numValues - valuesRemaining, valuesRemaining, compressedBuffer.get(),
            LBUG_PAGE_SIZE, EncodeExceptionView<T>{exceptionBufferCursor},
            remainingExceptionBufferSize, pageExceptionCount, metadata.compMeta);

        exceptionBufferCursor += pageExceptionCount * EncodeException<T>::sizeInBytes();
        remainingExceptionBufferSize -= pageExceptionCount * EncodeException<T>::sizeInBytes();
        totalExceptionCount += pageExceptionCount;

        // Avoid underflows (when data is compressed to nothing, numValuesPerPage may be
        // UINT64_MAX)
        if (numValuesPerPage > valuesRemaining) {
            valuesRemaining = 0;
        } else {
            valuesRemaining -= numValuesPerPage;
        }
        if (numPages >= entry.numPages - numExceptionPages ||
            static_cast<uint64_t>(entry.startPageIdx) + numPages >= dataFH->getNumPages())
            [[unlikely]] {
            throw StorageException("ALP data page range was exhausted during compression.");
        }
        dataFH->writePageToFile(compressedBuffer.get(), entry.startPageIdx + numPages);
        numPages++;
    }

    if (totalExceptionCount != floatMetadata.exceptionCount) [[unlikely]] {
        throw StorageException("ALP exception count does not match column metadata.");
    }

    return {std::move(exceptionBuffer), exceptionBufferSize};
}

template<std::floating_point T>
void flushALPExceptions(std::span<const uint8_t> exceptionBuffer, FileHandle* dataFH,
    const PageRange& entry, const ColumnChunkMetadata& metadata) {
    const auto encodedType = std::is_same_v<T, float> ? PhysicalTypeID::ALP_EXCEPTION_FLOAT :
                                                        PhysicalTypeID::ALP_EXCEPTION_DOUBLE;
    // we don't care about the min/max values for exceptions
    const auto& floatMetadata = FloatCompression<T>::getValidatedMetadata(metadata.compMeta);
    const auto preExceptionMetadata = uncompressedGetMetadata(encodedType,
        floatMetadata.exceptionCapacity, StorageValue{0}, StorageValue{0});
    const auto expectedExceptionBufferSize =
        static_cast<uint64_t>(preExceptionMetadata.getNumPages()) * LBUG_PAGE_SIZE;
    if (exceptionBuffer.size_bytes() < expectedExceptionBufferSize) [[unlikely]] {
        throw StorageException("ALP exception buffer is smaller than its metadata.");
    }

    if (preExceptionMetadata.getNumPages() > entry.numPages ||
        entry.startPageIdx == INVALID_PAGE_IDX ||
        static_cast<uint64_t>(entry.startPageIdx) + entry.numPages > dataFH->getNumPages())
        [[unlikely]] {
        throw StorageException("ALP exception page range is invalid during flush.");
    }

    const auto exceptionStartPageIdx =
        entry.startPageIdx + entry.numPages - preExceptionMetadata.getNumPages();
    PageRange exceptionBlock{exceptionStartPageIdx, preExceptionMetadata.getNumPages()};

    CompressedFlushBuffer exceptionFlushBuffer{
        std::make_shared<Uncompressed>(EncodeException<T>::sizeInBytes()), encodedType};
    (void)exceptionFlushBuffer.operator()(exceptionBuffer, dataFH, exceptionBlock,
        preExceptionMetadata);
}
} // namespace

template<std::floating_point T>
CompressedFloatFlushBuffer<T>::CompressedFloatFlushBuffer(std::shared_ptr<CompressionAlg> alg,
    PhysicalTypeID dataType)
    : alg{std::move(alg)}, dataType{dataType} {}

template<std::floating_point T>
CompressedFloatFlushBuffer<T>::CompressedFloatFlushBuffer(std::shared_ptr<CompressionAlg> alg,
    const LogicalType& dataType)
    : CompressedFloatFlushBuffer<T>(alg, dataType.getPhysicalType()) {}

template<std::floating_point T>
ColumnChunkMetadata CompressedFloatFlushBuffer<T>::operator()(std::span<const uint8_t> buffer,
    FileHandle* dataFH, const PageRange& entry, const ColumnChunkMetadata& metadata) const {
    if (metadata.compMeta.compression == CompressionType::UNCOMPRESSED) {
        return CompressedFlushBuffer{std::make_shared<Uncompressed>(dataType), dataType}.operator()(
            buffer, dataFH, entry, metadata);
    }
    if (metadata.compMeta.compression != CompressionType::ALP) [[unlikely]] {
        throw StorageException("ALP flush buffer received non-ALP compression metadata.");
    }

    auto [exceptionBuffer, exceptionBufferSize] =
        flushCompressedFloats<T>(*alg, dataType, buffer, dataFH, entry, metadata);

    flushALPExceptions<T>(std::span<const uint8_t>(exceptionBuffer.get(), exceptionBufferSize),
        dataFH, entry, metadata);

    return ColumnChunkMetadata(entry.startPageIdx, entry.numPages, metadata.numValues,
        metadata.compMeta);
}

template class CompressedFloatFlushBuffer<float>;
template class CompressedFloatFlushBuffer<double>;

} // namespace lbug::storage

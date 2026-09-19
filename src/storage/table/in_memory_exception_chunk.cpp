#include "storage/table/in_memory_exception_chunk.h"

#include <algorithm>
#include <limits>

#include "common/exception/storage.h"
#include "common/utils.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/compression/float_compression.h"
#include "storage/storage_utils.h"
#include "storage/table/column.h"
#include "storage/table/column_chunk_data.h"
#include <concepts>

namespace lbug::storage {

using namespace common;
using namespace transaction;

template<std::floating_point T>
using ExceptionInBuffer = std::array<std::byte, EncodeException<T>::sizeInBytes()>;

template<std::floating_point T>
InMemoryExceptionChunk<T>::InMemoryExceptionChunk(const SegmentState& state, FileHandle* dataFH,
    MemoryManager* memoryManager, ShadowFile* shadowFile)
    : exceptionCount(getValidatedMetadata(state).exceptionCount),
      finalizedExceptionCount(exceptionCount),
      exceptionCapacity(getValidatedMetadata(state).exceptionCapacity),
      emptyMask(exceptionCapacity),
      column(std::make_unique<Column>("ALPExceptionChunk", physicalType, dataFH, memoryManager,
          shadowFile, false, false /*has nulls*/)) {
    const auto& floatMetadata = getValidatedMetadata(state);
    const auto exceptionBaseCursor = getExceptionPageCursor(state.metadata,
        PageCursor{state.metadata.getStartPageIdx(), 0}, floatMetadata.exceptionCapacity);
    // for ALP exceptions we don't care about the statistics
    const auto compMeta =
        CompressionMetadata(StorageValue{0}, StorageValue{1}, CompressionType::UNCOMPRESSED);
    const auto exceptionChunkMeta = ColumnChunkMetadata(exceptionBaseCursor.pageIdx,
        safeIntegerConversion<page_idx_t>(
            EncodeException<T>::numPagesFromExceptions(exceptionCapacity)),
        exceptionCapacity, compMeta);
    chunkState = std::make_unique<SegmentState>(exceptionChunkMeta,
        EncodeException<T>::exceptionBytesPerPage() / EncodeException<T>::sizeInBytes());

    chunkData =
        std::make_unique<ColumnChunkData>(*memoryManager, physicalType, false, exceptionChunkMeta,
            false /*all written data is non-null and nulls are kept in a separate mask in-memory*/);
    chunkData->setToInMemory();
    column->scanSegment(*chunkState, chunkData.get(), 0, chunkState->metadata.numValues);
    validateInMemoryState();
}

template<std::floating_point T>
InMemoryExceptionChunk<T>::~InMemoryExceptionChunk() = default;

template<std::floating_point T>
void InMemoryExceptionChunk<T>::finalizeAndFlushToDisk(SegmentState& state) {
    const auto& floatMetadata = getValidatedMetadata(state);
    validateInMemoryState();
    finalize(state, floatMetadata);

    column->writeSegment(*chunkData, *chunkState, 0, *chunkData, 0, exceptionCapacity);
}

template<std::floating_point T>
void InMemoryExceptionChunk<T>::finalize(SegmentState& state, const ALPMetadata& floatMetadata) {
    if (floatMetadata.exceptionCapacity != exceptionCapacity) [[unlikely]] {
        throw StorageException("ALP exception capacity changed while the chunk was in memory.");
    }
    // removes holes + sorts exception chunk
    finalizedExceptionCount = 0;
    for (size_t i = 0; i < exceptionCount; ++i) {
        if (!emptyMask.isNull(i)) {
            ++finalizedExceptionCount;
            if (finalizedExceptionCount - 1 == i) {
                continue;
            }
            writeExceptionUnchecked(getExceptionAtUnchecked(i), finalizedExceptionCount - 1);
        }
    }

    if (finalizedExceptionCount > floatMetadata.exceptionCapacity) [[unlikely]] {
        throw StorageException("ALP exception count exceeds its allocated capacity.");
    }
    state.metadata.compMeta.floatMetadata()->exceptionCount = finalizedExceptionCount;

    if (finalizedExceptionCount > 1) {
        auto* exceptionWordBuffer = reinterpret_cast<ExceptionInBuffer<T>*>(chunkData->getData());
        std::sort(exceptionWordBuffer, exceptionWordBuffer + finalizedExceptionCount,
            [](ExceptionInBuffer<T>& a, ExceptionInBuffer<T>& b) {
                return EncodeExceptionView<T>{reinterpret_cast<std::byte*>(&a)}.getValue() <
                       EncodeExceptionView<T>{reinterpret_cast<std::byte*>(&b)}.getValue();
            });
    }
    if (exceptionCount > finalizedExceptionCount) {
        std::memset(chunkData->getData() +
                        finalizedExceptionCount * EncodeException<T>::sizeInBytes(),
            0, (exceptionCount - finalizedExceptionCount) * EncodeException<T>::sizeInBytes());
    }
    emptyMask.setNullFromRange(0, finalizedExceptionCount, false);
    emptyMask.setNullFromRange(finalizedExceptionCount, (exceptionCount - finalizedExceptionCount),
        true);
    exceptionCount = finalizedExceptionCount;
    chunkData->setNumValues(finalizedExceptionCount);
}

template<std::floating_point T>
void InMemoryExceptionChunk<T>::addException(EncodeException<T> exception) {
    validateInMemoryState();
    if (exceptionCount >= exceptionCapacity) [[unlikely]] {
        throw StorageException("ALP exception chunk has no remaining capacity.");
    }
    ++exceptionCount;
    writeExceptionUnchecked(exception, exceptionCount - 1);
    chunkData->setNumValues(exceptionCount);
    emptyMask.setNull(exceptionCount - 1, false);
}

template<std::floating_point T>
void InMemoryExceptionChunk<T>::removeExceptionAt(size_t exceptionIdx) {
    validateInMemoryState();
    validateExceptionIndex(exceptionIdx);
    // removing an exception does not free up space in the exception buffer
    emptyMask.setNull(exceptionIdx, true);
}

template<std::floating_point T>
EncodeException<T> InMemoryExceptionChunk<T>::getExceptionAt(size_t exceptionIdx) const {
    validateExceptionIndex(exceptionIdx);
    return getExceptionAtUnchecked(exceptionIdx);
}

template<std::floating_point T>
EncodeException<T> InMemoryExceptionChunk<T>::getExceptionAtUnchecked(size_t exceptionIdx) const {
    auto bytesInBuffer = chunkData->getValue<ExceptionInBuffer<T>>(exceptionIdx);
    return EncodeExceptionView<T>{reinterpret_cast<std::byte*>(&bytesInBuffer)}.getValue();
}

template<std::floating_point T>
void InMemoryExceptionChunk<T>::writeException(EncodeException<T> exception, size_t exceptionIdx) {
    validateExceptionIndex(exceptionIdx);
    writeExceptionUnchecked(exception, exceptionIdx);
}

template<std::floating_point T>
void InMemoryExceptionChunk<T>::writeExceptionUnchecked(EncodeException<T> exception,
    size_t exceptionIdx) {
    EncodeExceptionView<T>{reinterpret_cast<std::byte*>(chunkData->getData())}.setValue(exception,
        exceptionIdx);
}

template<std::floating_point T>
offset_t InMemoryExceptionChunk<T>::findFirstExceptionAtOrPastOffset(offset_t offsetInChunk) const {
    validateInMemoryState();
    // binary search for chunkOffset in exceptions
    // we only search among non-finalized exceptions

    offset_t lo = 0;
    offset_t hi = finalizedExceptionCount;
    while (lo < hi) {
        const size_t curExceptionIdx = (lo + hi) / 2;
        EncodeException<T> lastException = getExceptionAtUnchecked(curExceptionIdx);

        if (lastException.posInChunk < offsetInChunk) {
            lo = curExceptionIdx + 1;
        } else {
            hi = curExceptionIdx;
        }
    }

    return lo;
}

template<std::floating_point T>
PageCursor InMemoryExceptionChunk<T>::getExceptionPageCursor(const ColumnChunkMetadata& metadata,
    PageCursor pageBaseCursor, size_t exceptionCapacity) {
    const size_t numExceptionPages = EncodeException<T>::numPagesFromExceptions(exceptionCapacity);
    if (numExceptionPages > metadata.getNumPages()) [[unlikely]] {
        throw StorageException("ALP exception pages exceed the column chunk page range.");
    }
    const size_t exceptionPageOffset = metadata.getNumPages() - numExceptionPages;
    if (exceptionPageOffset != static_cast<page_idx_t>(exceptionPageOffset)) [[unlikely]] {
        throw StorageException("ALP exception page offset does not fit in a page index.");
    }
    if (pageBaseCursor.pageIdx == INVALID_PAGE_IDX ||
        pageBaseCursor.pageIdx > std::numeric_limits<page_idx_t>::max() -
                                     static_cast<page_idx_t>(exceptionPageOffset)) [[unlikely]] {
        throw StorageException("ALP exception page cursor exceeds the page index range.");
    }
    return {pageBaseCursor.pageIdx + (page_idx_t)exceptionPageOffset, 0};
}

template<std::floating_point T>
size_t InMemoryExceptionChunk<T>::getExceptionCount() const {
    return finalizedExceptionCount;
}

template<std::floating_point T>
void InMemoryExceptionChunk<T>::validateMetadata(const SegmentState& state) {
    const auto& metadata = state.metadata;
    if (metadata.compMeta.compression != CompressionType::ALP ||
        !metadata.compMeta.extraMetadata.has_value() ||
        dynamic_cast<const ALPMetadata*>(metadata.compMeta.extraMetadata.value().get()) ==
            nullptr) {
        throw StorageException("ALP column chunk has invalid compression metadata.");
    }
    if (metadata.compMeta.children.size() !=
        CompressionMetadata::getChildCount(CompressionType::ALP)) [[unlikely]] {
        throw StorageException("ALP column chunk has invalid child compression metadata.");
    }
    const auto childCompression = metadata.compMeta.children.front().compression;
    if (childCompression != CompressionType::CONSTANT &&
        childCompression != CompressionType::INTEGER_BITPACKING) [[unlikely]] {
        throw StorageException("ALP column chunk has an invalid child compression type.");
    }
    const auto* floatMetadata = metadata.compMeta.floatMetadata();
    if (floatMetadata->exceptionCount > floatMetadata->exceptionCapacity) [[unlikely]] {
        throw StorageException("ALP exception count exceeds its declared capacity.");
    }
    if (EncodeException<T>::numPagesFromExceptions(floatMetadata->exceptionCapacity) >
        metadata.getNumPages()) [[unlikely]] {
        throw StorageException("ALP exception capacity exceeds the column chunk page range.");
    }
    if (metadata.getNumPages() > 0 &&
        (metadata.getStartPageIdx() == INVALID_PAGE_IDX ||
            static_cast<uint64_t>(metadata.getStartPageIdx()) + metadata.getNumPages() >
                std::numeric_limits<page_idx_t>::max())) [[unlikely]] {
        throw StorageException("ALP column chunk page range is invalid.");
    }
}

template<std::floating_point T>
const ALPMetadata& InMemoryExceptionChunk<T>::getValidatedMetadata(const SegmentState& state) {
    validateMetadata(state);
    return *static_cast<const ALPMetadata*>(state.metadata.compMeta.extraMetadata.value().get());
}

template<std::floating_point T>
void InMemoryExceptionChunk<T>::validateInMemoryState() const {
    if (!column || !chunkData || !chunkState) [[unlikely]] {
        throw StorageException("ALP exception chunk is not initialized.");
    }
    const auto chunkNumValues = chunkData->getNumValues();
    const auto expectedNumPages = EncodeException<T>::numPagesFromExceptions(exceptionCapacity);
    if (exceptionCount > exceptionCapacity || finalizedExceptionCount > exceptionCount ||
        chunkData->getCapacity() < exceptionCapacity || chunkNumValues < exceptionCount ||
        chunkNumValues > exceptionCapacity ||
        chunkData->getDataType().getPhysicalType() != physicalType ||
        chunkData->getResidencyState() != ResidencyState::IN_MEMORY ||
        (exceptionCapacity > 0 && chunkData->getData() == nullptr) ||
        chunkState->metadata.numValues != exceptionCapacity ||
        chunkState->metadata.getNumPages() != expectedNumPages ||
        chunkState->metadata.compMeta.compression != CompressionType::UNCOMPRESSED) [[unlikely]] {
        throw StorageException("ALP exception chunk has inconsistent in-memory state.");
    }
}

template<std::floating_point T>
void InMemoryExceptionChunk<T>::validateExceptionIndex(size_t exceptionIdx) const {
    if (!chunkData || exceptionCount > exceptionCapacity || exceptionIdx >= exceptionCount ||
        exceptionIdx >= chunkData->getCapacity() || exceptionIdx >= chunkData->getNumValues() ||
        chunkData->getResidencyState() != ResidencyState::IN_MEMORY ||
        (exceptionCount > 0 && chunkData->getData() == nullptr)) [[unlikely]] {
        throw StorageException("ALP exception index is outside the exception chunk.");
    }
}

template class InMemoryExceptionChunk<float>;
template class InMemoryExceptionChunk<double>;

} // namespace lbug::storage

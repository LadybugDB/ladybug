#include "storage/table/dictionary_chunk.h"

#include "common/constants.h"
#include "common/exception/storage.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "storage/enums/residency_state.h"
#include <bit>

using namespace lbug::common;

namespace lbug {
namespace storage {

// The offset chunk is able to grow beyond the node group size.
// We rely on appending to the dictionary when updating, however if the chunk is full,
// there will be no space for in-place updates.
// The data chunk doubles in size on use, but out of place updates will never need the offset
// chunk to be greater than the node group size since they remove unused entries.
// So the chunk is initialized with a size equal to 3 so that the capacity is never resized to
// exactly the node group size (which is always a power of 2), making sure there is always extra
// space for updates.
static constexpr uint64_t INITIAL_OFFSET_CHUNK_CAPACITY = 3;

DictionaryChunk::DictionaryChunk(MemoryManager& mm, uint64_t capacity, bool enableCompression,
    ResidencyState residencyState)
    : enableCompression{enableCompression},
      indexTable(0, StringOps(this) /*hash*/, StringOps(this) /*equals*/) {
    // Bitpacking might save 1 bit per value with regular ascii compared to UTF-8
    stringDataChunk = ColumnChunkFactory::createColumnChunkData(mm, LogicalType::UINT8(),
        false /*enableCompression*/, 0, residencyState, false /*hasNullData*/);
    offsetChunk = ColumnChunkFactory::createColumnChunkData(mm, LogicalType::UINT64(),
        enableCompression, std::min(capacity, INITIAL_OFFSET_CHUNK_CAPACITY), residencyState,
        false /*hasNullData*/);
}

void DictionaryChunk::resetToEmpty() {
    stringDataChunk->resetToEmpty();
    offsetChunk->resetToEmpty();
    indexTable.clear();
}

uint64_t DictionaryChunk::getStringLength(string_index_t index) const {
    const auto range = getStringRange(index);
    return range.endOffset - range.startOffset;
}

DictionaryChunk::StringRange DictionaryChunk::getStringRange(string_index_t index) const {
    const auto numOffsets = offsetChunk->getNumValues();
    if (index >= numOffsets) [[unlikely]] {
        throw StorageException("String dictionary index is outside the offset table.");
    }

    const auto startOffset = offsetChunk->getValue<string_offset_t>(index);
    const auto nextIndex = static_cast<uint64_t>(index) + 1;
    const auto endOffset = nextIndex < numOffsets ?
                               offsetChunk->getValue<string_offset_t>(nextIndex) :
                               stringDataChunk->getNumValues();
    validateStringRange(startOffset, endOffset);
    return {startOffset, endOffset};
}

void DictionaryChunk::validateStringRange(string_offset_t startOffset,
    string_offset_t endOffset) const {
    const auto dataSize = stringDataChunk->getNumValues();
    if (startOffset > dataSize || endOffset > dataSize || endOffset < startOffset) [[unlikely]] {
        throw StorageException(
            "String dictionary contains a non-monotonic or out-of-range string offset.");
    }
}

DictionaryChunk::string_index_t DictionaryChunk::appendString(std::string_view val) {
    const auto found = indexTable.find(val);
    // If the string already exists in the dictionary, skip it and refer to the existing string
    if (enableCompression && found != indexTable.end()) {
        return found->index;
    }
    const auto leftSpace = stringDataChunk->getCapacity() - stringDataChunk->getNumValues();
    if (leftSpace < val.size()) {
        stringDataChunk->resize(std::bit_ceil(stringDataChunk->getCapacity() + val.size()));
    }
    const auto startOffset = stringDataChunk->getNumValues();
    memcpy(stringDataChunk->getData() + startOffset, val.data(), val.size());
    stringDataChunk->setNumValues(startOffset + val.size());
    const auto index = offsetChunk->getNumValues();
    if (index >= offsetChunk->getCapacity()) {
        offsetChunk->resize(offsetChunk->getCapacity() == 0 ?
                                2 :
                                (offsetChunk->getCapacity() * CHUNK_RESIZE_RATIO));
    }
    offsetChunk->setValue<string_offset_t>(startOffset, index);
    offsetChunk->setNumValues(index + 1);
    if (enableCompression) {
        indexTable.insert({static_cast<string_index_t>(index)});
    }
    return index;
}

std::string_view DictionaryChunk::getString(string_index_t index) const {
    const auto range = getStringRange(index);
    return std::string_view(reinterpret_cast<const char*>(stringDataChunk->getData()) +
                                range.startOffset,
        range.endOffset - range.startOffset);
}

bool DictionaryChunk::sanityCheck() const {
    if (!stringDataChunk->sanityCheck() || !offsetChunk->sanityCheck()) {
        return false;
    }

    const auto numOffsets = offsetChunk->getNumValues();
    if (numOffsets == 0) {
        return stringDataChunk->getNumValues() == 0;
    }

    // sanityCheck is an explicit diagnostic check, not part of the normal read/write path. Check
    // every range here, while getStringRange validates only the range that is actually consumed.
    try {
        if (offsetChunk->getValue<string_offset_t>(0) != 0) {
            return false;
        }
        for (uint64_t index = 0; index < numOffsets; ++index) {
            const auto startOffset = offsetChunk->getValue<string_offset_t>(index);
            const auto endOffset = index + 1 < numOffsets ?
                                       offsetChunk->getValue<string_offset_t>(index + 1) :
                                       stringDataChunk->getNumValues();
            validateStringRange(startOffset, endOffset);
        }
    } catch (const StorageException&) {
        return false;
    }
    return true;
}

void DictionaryChunk::resetNumValuesFromMetadata() {
    stringDataChunk->resetNumValuesFromMetadata();
    offsetChunk->resetNumValuesFromMetadata();
}

uint64_t DictionaryChunk::getEstimatedMemoryUsage() const {
    return stringDataChunk->getEstimatedMemoryUsage() + offsetChunk->getEstimatedMemoryUsage();
}

void DictionaryChunk::flush(PageAllocator& pageAllocator) {
    stringDataChunk->flush(pageAllocator);
    offsetChunk->flush(pageAllocator);
}

void DictionaryChunk::serialize(Serializer& serializer) const {
    serializer.writeDebuggingInfo("offset_chunk");
    offsetChunk->serialize(serializer);
    serializer.writeDebuggingInfo("string_data_chunk");
    stringDataChunk->serialize(serializer);
}

std::unique_ptr<DictionaryChunk> DictionaryChunk::deserialize(MemoryManager& memoryManager,
    Deserializer& deSer) {
    auto chunk = std::make_unique<DictionaryChunk>(memoryManager, 0, true, ResidencyState::ON_DISK);
    std::string key;
    deSer.validateDebuggingInfo(key, "offset_chunk");
    chunk->offsetChunk = ColumnChunkData::deserialize(memoryManager, deSer);
    deSer.validateDebuggingInfo(key, "string_data_chunk");
    chunk->stringDataChunk = ColumnChunkData::deserialize(memoryManager, deSer);
    // Keep dictionaries on disk after deserialization. Eagerly validating every offset here would
    // materialize and scan all dictionary pages during database open. Instead, all consumers
    // validate the range they use and fail closed with StorageException before dereferencing it.
    return chunk;
}

} // namespace storage
} // namespace lbug

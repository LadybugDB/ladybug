#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

#include "storage/enums/residency_state.h"
#include "storage/table/column_chunk_data.h"

namespace lbug {
namespace storage {
class MemoryManager;

class DictionaryChunk {
public:
    using string_offset_t = uint64_t;
    using string_index_t = uint32_t;

    DictionaryChunk(MemoryManager& mm, uint64_t capacity, bool enableCompression,
        ResidencyState residencyState);
    // A pointer to the dictionary chunk is stored in the indexTable for key comparisons
    // and can't be modified easily. Moving would invalidate that pointer
    DictionaryChunk(DictionaryChunk&& other) = delete;

    void setToInMemory() {
        stringDataChunk->setToInMemory();
        offsetChunk->setToInMemory();
        indexTable.clear();
    }
    void resetToEmpty();

    uint64_t getStringLength(string_index_t index) const;

    string_index_t appendString(std::string_view val);

    std::string_view getString(string_index_t index) const;

    ColumnChunkData* getStringDataChunk() const { return stringDataChunk.get(); }
    ColumnChunkData* getOffsetChunk() const { return offsetChunk.get(); }
    void setOffsetChunk(std::unique_ptr<ColumnChunkData> chunk) { offsetChunk = std::move(chunk); }
    void setStringDataChunk(std::unique_ptr<ColumnChunkData> chunk) {
        stringDataChunk = std::move(chunk);
    }

    void resetNumValuesFromMetadata();

    bool sanityCheck() const;

    uint64_t getEstimatedMemoryUsage() const;

    void serialize(common::Serializer& serializer) const;
    static std::unique_ptr<DictionaryChunk> deserialize(MemoryManager& memoryManager,
        common::Deserializer& deSer);

    void flush(PageAllocator& pageAllocator);

private:
    struct StringRange {
        string_offset_t startOffset;
        string_offset_t endOffset;
    };

    // Read and validate both offsets once. Keeping the validated range together prevents callers
    // from re-reading an offset after validation.
    StringRange getStringRange(string_index_t index) const;

    // Validate the range before constructing a string_view. This must not be a debug-only
    // assertion: dictionaries are persisted and a corrupt offset otherwise becomes an unsigned
    // underflow that is later used as a string_view length.
    void validateStringRange(string_offset_t startOffset, string_offset_t endOffset) const;

    bool enableCompression;
    // String data is stored as a UINT8 chunk, using the numValues in the chunk to track the number
    // of characters stored.
    std::unique_ptr<ColumnChunkData> stringDataChunk;
    std::unique_ptr<ColumnChunkData> offsetChunk;

    // Open-addressing hash set of dictionary indexes, keyed by string content. Replaces a
    // node-based std::unordered_set (gh-1071): one malloc per distinct string plus
    // pointer-chasing lookups made COPY of STRING columns grow faster than the row count once
    // the table outgrew the cache. Flat slots with cached hashes keep probes contiguous and
    // rehashes free of string comparisons.
    class DictionaryIndexTable {
    public:
        explicit DictionaryIndexTable(const DictionaryChunk* dict) : dict{dict} {}

        void clear() {
            slots.clear();
            numElements = 0;
        }

        std::optional<string_index_t> find(std::string_view key) const {
            if (slots.empty()) {
                return std::nullopt;
            }
            const auto hash = hashKey(key);
            auto pos = hash & mask();
            while (true) {
                const auto& slot = slots[pos];
                if (!slot.occupied) {
                    return std::nullopt;
                }
                if (slot.hash == hash && dict->getString(slot.index) == key) {
                    return slot.index;
                }
                pos = (pos + 1) & mask();
            }
        }

        void insert(string_index_t index) {
            const auto key = dict->getString(index);
            const auto hash = hashKey(key);
            growForInsert();
            auto pos = hash & mask();
            while (true) {
                auto& slot = slots[pos];
                if (!slot.occupied) {
                    slot.hash = hash;
                    slot.index = index;
                    slot.occupied = true;
                    numElements++;
                    return;
                }
                // Same de-duplication semantics as the unordered_set this replaces:
                // inserting an already-present value keeps the existing entry.
                if (slot.hash == hash && dict->getString(slot.index) == key) {
                    return;
                }
                pos = (pos + 1) & mask();
            }
        }

    private:
        struct Slot {
            std::size_t hash = 0;
            string_index_t index = 0;
            bool occupied = false;
        };
        // Keep the load factor at or below this ratio; probes stay short.
        static constexpr double MAX_LOAD_FACTOR = 0.7;
        static constexpr uint64_t MIN_CAPACITY = 16;

        static std::size_t hashKey(std::string_view key) {
            return std::hash<std::string_view>{}(key);
        }

        uint64_t mask() const { return slots.size() - 1; }

        void growForInsert() {
            if (!slots.empty() &&
                (double)(numElements + 1) <= (double)slots.size() * MAX_LOAD_FACTOR) {
                return;
            }
            uint64_t newCapacity = slots.empty() ? MIN_CAPACITY : slots.size() * 2;
            while ((double)(numElements + 1) > (double)newCapacity * MAX_LOAD_FACTOR) {
                newCapacity *= 2;
            }
            std::vector<Slot> newSlots(newCapacity);
            const auto newMask = newCapacity - 1;
            for (auto& slot : slots) {
                if (!slot.occupied) {
                    continue;
                }
                auto pos = slot.hash & newMask;
                while (newSlots[pos].occupied) {
                    pos = (pos + 1) & newMask;
                }
                newSlots[pos] = slot;
            }
            slots = std::move(newSlots);
        }

        const DictionaryChunk* dict;
        std::vector<Slot> slots;
        uint64_t numElements = 0;
    };

    DictionaryIndexTable indexTable;
};
} // namespace storage
} // namespace lbug

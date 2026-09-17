#pragma once

#include "common/exception/runtime.h"
#include "hash_index_slot.h"
#include <format>

namespace lbug {
namespace storage {

struct HashIndexHeaderOnDisk {
    explicit HashIndexHeaderOnDisk()
        : nextSplitSlotId{0}, numEntries{0},
          firstFreeOverflowSlotId{SlotHeader::INVALID_OVERFLOW_SLOT_ID}, currentLevel{0} {}
    slot_id_t nextSplitSlotId;
    uint64_t numEntries;
    slot_id_t firstFreeOverflowSlotId;
    uint8_t currentLevel;
    uint8_t _padding[7]{};
};
static_assert(std::has_unique_object_representations_v<HashIndexHeaderOnDisk>);

class HashIndexHeader {
public:
    static constexpr uint64_t MAX_CURRENT_LEVEL = 63;

    explicit HashIndexHeader()
        : currentLevel{1}, levelHashMask{1}, higherLevelHashMask{3}, nextSplitSlotId{0},
          numEntries{0}, firstFreeOverflowSlotId{SlotHeader::INVALID_OVERFLOW_SLOT_ID} {}

    explicit HashIndexHeader(const HashIndexHeaderOnDisk& onDiskHeader)
        : currentLevel{onDiskHeader.currentLevel}, levelHashMask{getHashMask(this->currentLevel)},
          higherLevelHashMask{getHashMask(this->currentLevel + 1)},
          nextSplitSlotId{onDiskHeader.nextSplitSlotId}, numEntries{onDiskHeader.numEntries},
          firstFreeOverflowSlotId{onDiskHeader.firstFreeOverflowSlotId} {
        validateOnDisk(onDiskHeader);
    }

    static void validateOnDisk(const HashIndexHeaderOnDisk& onDiskHeader) {
        if (onDiskHeader.currentLevel > MAX_CURRENT_LEVEL) {
            throw common::RuntimeException(std::format(
                "Invalid hash index header: current level {} is out of bounds. The database file "
                "may be corrupted.",
                onDiskHeader.currentLevel));
        }
        if (onDiskHeader.nextSplitSlotId >= (1ull << onDiskHeader.currentLevel)) {
            throw common::RuntimeException(std::format(
                "Invalid hash index header: next split slot ID {} is invalid for current level {}. "
                "The database file may be corrupted.",
                onDiskHeader.nextSplitSlotId, onDiskHeader.currentLevel));
        }
    }

    inline void incrementLevel() {
        if (currentLevel >= MAX_CURRENT_LEVEL) {
            throw common::RuntimeException(
                "Cannot increase hash index header level: the maximum level was reached.");
        }
        currentLevel++;
        nextSplitSlotId = 0;
        levelHashMask = getHashMask(currentLevel);
        higherLevelHashMask = getHashMask(currentLevel + 1);
    }
    inline void incrementNextSplitSlotId() {
        if (nextSplitSlotId < (1ull << currentLevel) - 1) {
            nextSplitSlotId++;
        } else {
            incrementLevel();
        }
    }

    inline void write(HashIndexHeaderOnDisk& onDiskHeader) const {
        onDiskHeader.currentLevel = currentLevel;
        onDiskHeader.nextSplitSlotId = nextSplitSlotId;
        onDiskHeader.numEntries = numEntries;
        onDiskHeader.firstFreeOverflowSlotId = firstFreeOverflowSlotId;
    }

private:
    static constexpr uint64_t getHashMask(uint64_t level) {
        // A shift by 64 is undefined, while a level of 63 is valid for a uint64_t hash.
        return level >= 64 ? UINT64_MAX : (1ull << level) - 1;
    }

public:
    uint64_t currentLevel;
    uint64_t levelHashMask;
    uint64_t higherLevelHashMask;
    // Id of the next slot to split when resizing the hash index
    slot_id_t nextSplitSlotId;
    uint64_t numEntries;
    // Id of the first in a chain of empty overflow slots which have been reclaimed during slot
    // splitting. The nextOvfSlotId field in the slot's header indicates the next slot in the chain.
    // These slots should be used first when allocating new overflow slots
    // TODO(bmwinger): Make use of this in the on-disk hash index
    slot_id_t firstFreeOverflowSlotId;
};

} // namespace storage
} // namespace lbug

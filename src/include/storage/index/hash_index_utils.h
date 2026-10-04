#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "common/constants.h"
#include "common/system_config.h"
#include "common/types/string_t.h"
#include "common/types/types.h"
#include "function/hash/hash_functions.h"
#include "storage/index/hash_index_header.h"

namespace lbug {
namespace storage {

static constexpr uint64_t NUM_HASH_INDEXES = common::HashIndexConstants::NUM_HASH_INDEXES;
static constexpr uint64_t NUM_HASH_INDEXES_LOG2 = common::HashIndexConstants::NUM_HASH_INDEXES_LOG2;

static constexpr common::page_idx_t INDEX_HEADER_PAGES = 2;
static constexpr uint64_t INDEX_HEADERS_PER_PAGE =
    common::LBUG_PAGE_SIZE / sizeof(HashIndexHeaderOnDisk);

static constexpr common::page_idx_t P_SLOTS_HEADER_PAGE_IDX = 0;
static constexpr common::page_idx_t O_SLOTS_HEADER_PAGE_IDX = 1;
static constexpr common::page_idx_t NUM_HEADER_PAGES = 2;
static constexpr uint64_t INDEX_HEADER_IDX_IN_ARRAY = 0;

// so that all 256 hash indexes can be stored in two pages, the HashIndexHeaderOnDisk must be
// smaller than 32 bytes
static_assert(NUM_HASH_INDEXES * sizeof(HashIndexHeaderOnDisk) <=
              common::LBUG_PAGE_SIZE * INDEX_HEADER_PAGES);

enum class SlotType : uint8_t { PRIMARY = 0, OVF = 1 };

struct SlotInfo {
    slot_id_t slotId{UINT64_MAX};
    SlotType slotType{SlotType::PRIMARY};

    bool operator==(const SlotInfo&) const = default;
};

class HashIndexUtils {

public:
    static constexpr auto INVALID_OVF_INFO =
        SlotInfo{SlotHeader::INVALID_OVERFLOW_SLOT_ID, SlotType::OVF};

    static bool areStringPrefixAndLenEqual(std::string_view keyToLookup,
        const common::string_t& keyInEntry) {
        auto prefixLen =
            std::min(static_cast<uint64_t>(keyInEntry.len), common::string_t::PREFIX_LENGTH);
        return keyToLookup.length() == keyInEntry.len &&
               memcmp(keyToLookup.data(), keyInEntry.prefix, prefixLen) == 0;
    }

    template<typename T>
    static common::hash_t hash(const T& key) {
        common::hash_t hash = 0;
        function::Hash::operation(key, hash);
        return hash;
    }

    // Pre-0.21 string hash, solely for detecting stale persisted indexes (issue #1092).
    // Before the char-signedness portability fix (issue #882, v0.21.0), the trailing size%8
    // bytes were accumulated through plain (possibly signed) char, sign-extending bytes >=
    // 0x80 on signed-char platforms (x86-64, macOS arm64, wasm32). String-key hash indexes
    // persisted by <= 0.20.x on those platforms therefore placed non-ASCII keys in slots the
    // current hash function never probes. This replicates those legacy values exactly (the
    // 8-byte blocks use memcpy instead of the original misaligned uint64_t load, which is
    // value-identical and avoids reintroducing issue #879). Do NOT use for any new hashing.
    static common::hash_t hashStringPre021(std::string_view key) {
        common::hash_t hashValue = 0;
        for (size_t i = 0u; i < key.size() / 8; i++) {
            uint64_t block = 0;
            memcpy(&block, key.data() + i * sizeof(block), sizeof(block));
            hashValue = function::combineHashScalar(hashValue, function::murmurhash64(block));
        }
        uint64_t last = 0;
        for (size_t i = 0u; i < key.size() % 8; i++) {
            last |= static_cast<uint64_t>(key[key.size() / 8 * 8 + i]) << i * 8;
        }
        return function::combineHashScalar(hashValue, function::murmurhash64(last));
    }

    static uint8_t getFingerprintForHash(common::hash_t hash) {
        // Last 8 bits before the bits used to calculate the hash index position is the fingerprint
        return (hash >> (64 - NUM_HASH_INDEXES_LOG2 - 8)) & 255;
    }

    static slot_id_t getPrimarySlotIdForHash(const HashIndexHeader& indexHeader,
        common::hash_t hash) {
        auto slotId = hash & indexHeader.levelHashMask;
        if (slotId < indexHeader.nextSplitSlotId) {
            slotId = hash & indexHeader.higherLevelHashMask;
        }
        return slotId;
    }

    static uint64_t getHashIndexPosition(common::IndexHashable auto key) {
        return (HashIndexUtils::hash(key) >> (64 - NUM_HASH_INDEXES_LOG2)) & (NUM_HASH_INDEXES - 1);
    }

    static uint64_t getNumRequiredEntries(uint64_t numEntries) {
        return ceil(static_cast<double>(numEntries) * common::DEFAULT_HT_LOAD_FACTOR);
    }
};
} // namespace storage
} // namespace lbug

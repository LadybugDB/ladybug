#pragma once

#include <cstdint>

#include "common/types/types.h"
#include "storage/wal/record/wal_record_base.h"

namespace lbug {
namespace storage {

struct UpdateSequenceRecord final : WALRecord {
    common::sequence_id_t sequenceID;
    uint64_t kCount;

    UpdateSequenceRecord()
        : WALRecord{WALRecordType::UPDATE_SEQUENCE_RECORD}, sequenceID{0}, kCount{0} {}
    UpdateSequenceRecord(common::sequence_id_t sequenceID, uint64_t kCount)
        : WALRecord{WALRecordType::UPDATE_SEQUENCE_RECORD}, sequenceID{sequenceID}, kCount{kCount} {
    }

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<UpdateSequenceRecord> deserialize(common::Deserializer& deserializer);
};

} // namespace storage
} // namespace lbug

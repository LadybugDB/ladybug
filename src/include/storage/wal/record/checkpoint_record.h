#pragma once

#include "storage/wal/record/wal_record_base.h"

namespace lbug {
namespace storage {

struct CheckpointRecord final : WALRecord {
    CheckpointRecord() : WALRecord{WALRecordType::CHECKPOINT_RECORD} {}

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<CheckpointRecord> deserialize(common::Deserializer& deserializer);
};

} // namespace storage
} // namespace lbug

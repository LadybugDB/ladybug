#pragma once

#include "storage/wal/record/wal_record_base.h"

namespace lbug {
namespace storage {

struct CommitRecord final : WALRecord {
    CommitRecord() : WALRecord{WALRecordType::COMMIT_RECORD} {}

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<CommitRecord> deserialize(common::Deserializer& deserializer);
};

} // namespace storage
} // namespace lbug

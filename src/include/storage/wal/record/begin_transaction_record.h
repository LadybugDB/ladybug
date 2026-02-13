#pragma once

#include "storage/wal/record/wal_record_base.h"

namespace lbug {
namespace storage {

struct BeginTransactionRecord final : WALRecord {
    BeginTransactionRecord() : WALRecord{WALRecordType::BEGIN_TRANSACTION_RECORD} {}

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<BeginTransactionRecord> deserialize(common::Deserializer& deserializer);
};

} // namespace storage
} // namespace lbug

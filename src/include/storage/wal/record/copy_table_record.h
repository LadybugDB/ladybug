#pragma once

#include "common/types/types.h"
#include "storage/wal/record/wal_record_base.h"

namespace lbug {
namespace storage {

struct CopyTableRecord final : WALRecord {
    common::table_id_t tableID;

    CopyTableRecord()
        : WALRecord{WALRecordType::COPY_TABLE_RECORD}, tableID{common::INVALID_TABLE_ID} {}
    explicit CopyTableRecord(common::table_id_t tableID)
        : WALRecord{WALRecordType::COPY_TABLE_RECORD}, tableID{tableID} {}

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<CopyTableRecord> deserialize(common::Deserializer& deserializer);
};

} // namespace storage
} // namespace lbug

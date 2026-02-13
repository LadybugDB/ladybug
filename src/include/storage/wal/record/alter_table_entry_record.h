#pragma once

#include "binder/ddl/bound_alter_info.h"
#include "storage/wal/record/wal_record_base.h"

namespace lbug {
namespace storage {

struct AlterTableEntryRecord final : WALRecord {
    const binder::BoundAlterInfo* alterInfo;
    std::unique_ptr<binder::BoundAlterInfo> ownedAlterInfo;

    AlterTableEntryRecord()
        : WALRecord{WALRecordType::ALTER_TABLE_ENTRY_RECORD}, alterInfo{nullptr} {}
    explicit AlterTableEntryRecord(const binder::BoundAlterInfo* alterInfo)
        : WALRecord{WALRecordType::ALTER_TABLE_ENTRY_RECORD}, alterInfo{alterInfo} {}

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<AlterTableEntryRecord> deserialize(common::Deserializer& deserializer);
};

} // namespace storage
} // namespace lbug

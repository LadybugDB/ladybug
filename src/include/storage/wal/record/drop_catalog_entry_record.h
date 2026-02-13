#pragma once

#include "catalog/catalog_entry/catalog_entry_type.h"
#include "common/types/types.h"
#include "storage/wal/record/wal_record_base.h"

namespace lbug {
namespace storage {

struct DropCatalogEntryRecord final : WALRecord {
    common::oid_t entryID;
    catalog::CatalogEntryType entryType;

    DropCatalogEntryRecord()
        : WALRecord{WALRecordType::DROP_CATALOG_ENTRY_RECORD}, entryID{common::INVALID_OID},
          entryType{} {}
    DropCatalogEntryRecord(common::table_id_t entryID, catalog::CatalogEntryType entryType)
        : WALRecord{WALRecordType::DROP_CATALOG_ENTRY_RECORD}, entryID{entryID},
          entryType{entryType} {}

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<DropCatalogEntryRecord> deserialize(common::Deserializer& deserializer);
};

} // namespace storage
} // namespace lbug

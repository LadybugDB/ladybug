#pragma once

#include "catalog/catalog_entry/catalog_entry.h"
#include "storage/wal/record/wal_record_base.h"

namespace lbug {
namespace storage {

struct CreateCatalogEntryRecord final : WALRecord {
    catalog::CatalogEntry* catalogEntry;
    std::unique_ptr<catalog::CatalogEntry> ownedCatalogEntry;
    bool isInternal = false;

    CreateCatalogEntryRecord()
        : WALRecord{WALRecordType::CREATE_CATALOG_ENTRY_RECORD}, catalogEntry{nullptr} {}
    CreateCatalogEntryRecord(catalog::CatalogEntry* catalogEntry, bool isInternal)
        : WALRecord{WALRecordType::CREATE_CATALOG_ENTRY_RECORD}, catalogEntry{catalogEntry},
          isInternal{isInternal} {}

    void serialize(common::Serializer& serializer) const override;
    static std::unique_ptr<CreateCatalogEntryRecord> deserialize(
        common::Deserializer& deserializer);
};

} // namespace storage
} // namespace lbug

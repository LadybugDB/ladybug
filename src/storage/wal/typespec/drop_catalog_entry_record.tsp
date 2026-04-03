import "common/types/types.h";
import "catalog/catalog_entry/catalog_entry_type.h";

model DropCatalogEntryRecord {
    entryID: common::oid_t
    entryType: catalog::CatalogEntryType
}

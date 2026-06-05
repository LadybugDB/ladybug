// @defaults=entryID:common::INVALID_OID,entryType:{}
// @debug_fields=false
import "../common.tsp";
import "catalog/catalog_entry/catalog_entry_type.h";
import "common/types/types.h";

model DropCatalogEntryRecord extends WalRecordBase {
    entryID: oid_t;
    entryType: CatalogEntryType;
}

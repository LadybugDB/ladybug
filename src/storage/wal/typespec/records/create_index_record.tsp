// Physical built-in index creation payload. The C++ template keeps the concrete
// IndexCatalogEntry/IndexInfo/tree-byte serialization because these are Ladybug
// storage types rather than portable TypeSpec models.
import "../common.tsp";
import "catalog/catalog_entry/catalog_entry.h";
import "storage/index/index.h";

@discriminated(#{discriminatorPropertyName: "entryType", envelope: "none"})
union CatalogEntryPayload {
    index: IndexCatalogEntryPayload,
}

model IndexCatalogEntryPayload {
    entryType: CatalogEntryType;
    name: string;
}

model CreateIndexRecord extends WalRecordBase {
    catalogEntry: CatalogEntryPayload;
}

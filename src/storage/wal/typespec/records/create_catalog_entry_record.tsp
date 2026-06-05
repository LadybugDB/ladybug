// CatalogEntry serialization is polymorphic through a custom template branch.
import "../common.tsp";
import "catalog/catalog_entry/catalog_entry.h";

// CatalogEntry::serialize writes a type tag followed by subclass-specific
// payload. The TypeSpec union records that shape while the C++ template keeps
// using CatalogEntry::serialize/deserialize and the replay-side owned pointer.
@discriminated(#{discriminatorPropertyName: "entryType", envelope: "none"})
union CatalogEntryPayload {
    nodeTable: TableCatalogEntryPayload,
    relGroup: TableCatalogEntryPayload,
    scalarMacro: ScalarMacroCatalogEntryPayload,
    sequence: SequenceCatalogEntryPayload,
    typeEntry: TypeCatalogEntryPayload,
    index: IndexCatalogEntryPayload,
    graph: GraphCatalogEntryPayload,
}

model TableCatalogEntryPayload {
    entryType: CatalogEntryType;
    name: string;
}

model ScalarMacroCatalogEntryPayload {
    entryType: CatalogEntryType;
    name: string;
}

model SequenceCatalogEntryPayload {
    entryType: CatalogEntryType;
    name: string;
}

model TypeCatalogEntryPayload {
    entryType: CatalogEntryType;
    name: string;
}

model IndexCatalogEntryPayload {
    entryType: CatalogEntryType;
    name: string;
}

model GraphCatalogEntryPayload {
    entryType: CatalogEntryType;
    name: string;
    isAnyGraph: boolean;
}

model CreateCatalogEntryRecord extends WalRecordBase {
    catalogEntry: CatalogEntryPayload;
    isInternal: boolean;
}

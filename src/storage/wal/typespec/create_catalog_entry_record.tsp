import "catalog/catalog_entry/catalog_entry.h";

// Constructor order: implicit ptr fields come before regular fields.
@ptrFirst
@noOwnCtors
model CreateCatalogEntryRecord {
    isInternal: bool
    // @implicitPtr: catalog::CatalogEntry catalogEntry
}

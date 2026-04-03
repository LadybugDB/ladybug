import "binder/ddl/bound_alter_info.h";
import <memory>;

// Only raw pointer, no ownership overloads.
@explicit
model AlterTableEntryRecord {
    // @implicitRawPtr: const binder::BoundAlterInfo alterInfo
}

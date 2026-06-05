// BoundAlterInfo serializes variant extraInfo by alter type through a custom template branch.
import "../common.tsp";
import "binder/ddl/bound_alter_info.h";

// These models document the variant wire payload. The C++ generator still emits
// a semi-manual switch because each variant maps to a specific BoundExtraAlterInfo
// subclass and construction API.
model BoundAlterInfoPayload {
    alterType: AlterType;
    tableName: string;
    extraInfo: AlterExtraInfo;
}

@discriminated(#{discriminatorPropertyName: "alterType", envelope: "none"})
union AlterExtraInfo {
    addProperty: AddPropertyAlterPayload,
    dropProperty: DropPropertyAlterPayload,
    renameProperty: RenamePropertyAlterPayload,
    comment: CommentAlterPayload,
    rename: RenameTableAlterPayload,
    addFromToConnection: AddFromToConnectionAlterPayload,
    dropFromToConnection: DropFromToConnectionAlterPayload,
}

model AddPropertyAlterPayload {
    propertyDefinition: PropertyDefinition;
}

model DropPropertyAlterPayload {
    propertyName: string;
}

model RenamePropertyAlterPayload {
    newName: string;
    oldName: string;
}

model CommentAlterPayload {
    comment: string;
}

model RenameTableAlterPayload {
    newName: string;
}

model AddFromToConnectionAlterPayload {
    fromTableID: table_id_t;
    toTableID: table_id_t;
}

model DropFromToConnectionAlterPayload {
    fromTableID: table_id_t;
    toTableID: table_id_t;
}

model AlterTableEntryRecord extends WalRecordBase {
    alterInfo: BoundAlterInfoPayload;
}

// @defaults=tableID:common::INVALID_TABLE_ID
// @debug_fields=false
import "../common.tsp";
import "common/types/types.h";

model CopyTableRecord extends WalRecordBase {
    tableID: table_id_t;
}

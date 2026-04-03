// rel_detach_delete_record.tsp
import "common/enums/rel_direction.h";
import "common/types/types.h";
import "common/vector/value_vector.h";

model RelDetachDeleteRecord {
    tableID: common::table_id_t
    direction: common::RelDataDirection
    // @implicitPtr: common::ValueVector srcNodeIDVector
}

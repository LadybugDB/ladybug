// node_update_record.tsp
import "common/types/types.h";
import "common/vector/value_vector.h";

model NodeUpdateRecord {
    tableID: common::table_id_t
    columnID: common::column_id_t
    nodeOffset: common::offset_t
    // @implicitPtr: common::ValueVector propertyVector
}

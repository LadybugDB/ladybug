// node_deletion_record.tsp
import "common/types/types.h";
import "common/vector/value_vector.h";

model NodeDeletionRecord {
    tableID: common::table_id_t
    nodeOffset: common::offset_t
    // @implicitPtr: common::ValueVector pkVector
}

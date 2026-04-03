// rel_update_record.tsp
import "common/types/types.h";
import "common/vector/value_vector.h";

model RelUpdateRecord {
    tableID: common::table_id_t
    columnID: common::column_id_t
    // @implicitPtrs: common::ValueVector srcNodeIDVector, common::ValueVector dstNodeIDVector, common::ValueVector relIDVector, common::ValueVector propertyVector
}

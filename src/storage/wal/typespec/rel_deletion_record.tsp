// rel_deletion_record.tsp
import "common/types/types.h";
import "common/vector/value_vector.h";

model RelDeletionRecord {
    tableID: common::table_id_t
    // @implicitPtrs: common::ValueVector srcNodeIDVector, common::ValueVector dstNodeIDVector, common::ValueVector relIDVector
}

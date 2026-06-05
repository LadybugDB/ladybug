import "../common.tsp";
import <utility>;
import "common/types/types.h";
import "common/vector/value_vector.h";
// @wire_names=tableID:table_id,columnID:column_id,srcNodeIDVector:src_node_vector,dstNodeIDVector:dst_node_vector,relIDVector:rel_id_vector,propertyVector:property_vector

model RelUpdateRecord extends WalRecordBase {
    tableID: table_id_t;
    columnID: column_id_t;
    srcNodeIDVector: ValueVector;
    dstNodeIDVector: ValueVector;
    relIDVector: ValueVector;
    propertyVector: ValueVector;
}

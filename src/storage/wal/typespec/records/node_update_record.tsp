import "../common.tsp";
import <utility>;
import "common/types/types.h";
import "common/vector/value_vector.h";
// @wire_names=tableID:table_id,columnID:column_id,nodeOffset:node_offset,propertyVector:property_vector

model NodeUpdateRecord extends WalRecordBase {
    tableID: table_id_t;
    columnID: column_id_t;
    nodeOffset: offset_t;
    propertyVector: ValueVector;
}

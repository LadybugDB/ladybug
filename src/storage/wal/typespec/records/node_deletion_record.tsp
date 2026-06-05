import "../common.tsp";
import <utility>;
import "common/types/types.h";
import "common/vector/value_vector.h";
// @wire_names=tableID:table_id,nodeOffset:node_offset,pkVector:pk_vector
// @owned_names=pkVector:ownedPKVector

model NodeDeletionRecord extends WalRecordBase {
    tableID: table_id_t;
    nodeOffset: offset_t;
    pkVector: ValueVector;
}

import "../common.tsp";
import <utility>;
import "common/enums/rel_direction.h";
import "common/types/types.h";
import "common/vector/value_vector.h";
// @wire_names=tableID:table_id,direction:direction,srcNodeIDVector:src_node_vector

model RelDetachDeleteRecord extends WalRecordBase {
    tableID: table_id_t;
    direction: RelDataDirection;
    srcNodeIDVector: ValueVector;
}

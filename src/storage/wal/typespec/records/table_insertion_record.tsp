// ValueVector[] maps to std::vector<ValueVector*> while serialization writes
// each ValueVector payload, mirroring DuckDB's vector/chunk serialization shape
// with Ladybug's ValueVector::deSerialize and DataChunkState.
import "../common.tsp";
import <utility>;
import <vector>;
import "common/enums/table_type.h";
import "common/types/types.h";
import "common/vector/value_vector.h";
// @wire_names=tableID:table_id,tableType:table_type,numRows:num_rows,vectors:num_vectors

model TableInsertionRecord extends WalRecordBase {
    tableID: table_id_t;
    tableType: TableType;
    numRows: row_idx_t;
    vectors: ValueVector[];
}

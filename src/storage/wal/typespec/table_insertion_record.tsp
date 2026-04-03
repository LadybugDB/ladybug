// table_insertion_record.tsp
import "common/enums/table_type.h";
import "common/types/types.h";
import "common/vector/value_vector.h";

model TableInsertionRecord {
    tableID: common::table_id_t
    tableType: common::TableType
    numRows: common::row_idx_t
    // @implicitVec: common::ValueVector
}

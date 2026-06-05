// @debug_fields=false
import "../common.tsp";
import <cstdint>;
import "common/types/types.h";

model UpdateSequenceRecord extends WalRecordBase {
    sequenceID: sequence_id_t;
    kCount: uint64_t;
}

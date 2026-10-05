// @record_type=UPDATE_SEQUENCE_NAMED_RECORD
// @debug_fields=false
import "../common.tsp";
import <cstdint>;
import <string>;
import <utility>;
import "common/types/types.h";

model UpdateSequenceNamedRecord extends WalRecordBase {
    sequenceID: sequence_id_t;
    kCount: uint64_t;
    sequenceName: string;
}

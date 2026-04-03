// update_sequence_record.tsp
import <cstdint>;
import "common/types/types.h";

model UpdateSequenceRecord {
    sequenceID: common::sequence_id_t
    kCount: uint64_t
}

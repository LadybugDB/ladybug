import "../common.tsp";
import <cstdint>;

// @explicit_constructor=true
model AbortRecord extends WalRecordBase {
    abortedBlockStart: uint64_t;
}

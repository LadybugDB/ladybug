import "../common.tsp";
import <cstdint>;

// @explicit_constructor=true
model CheckpointRecord extends WalRecordBase {
    bundleFormatVersion: uint64_t;
}

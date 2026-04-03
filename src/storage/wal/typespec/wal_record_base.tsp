// wal_record_base.tsp
//
// Defines the WALRecordType enum, WALHeader, and the abstract WALRecord base.
//
// @include: common/cast.h
// @include: common/copy_constructors.h
// @include: common/types/uuid.h
// @headerComment("This is not used for any record. 0 is reserved to detect cases where we accidentally read from an empty buffer.")

enum WALRecordType : uint8_t {
    INVALID_RECORD = 0,
    BEGIN_TRANSACTION_RECORD = 1,
    COMMIT_RECORD = 2,
    COPY_TABLE_RECORD = 13,
    CREATE_CATALOG_ENTRY_RECORD = 14,
    DROP_CATALOG_ENTRY_RECORD = 16,
    ALTER_TABLE_ENTRY_RECORD = 17,
    UPDATE_SEQUENCE_RECORD = 18,
    TABLE_INSERTION_RECORD = 30,
    NODE_DELETION_RECORD = 31,
    NODE_UPDATE_RECORD = 32,
    REL_DELETION_RECORD = 33,
    REL_DETACH_DELETE_RECORD = 34,
    REL_UPDATE_RECORD = 35,
    LOAD_EXTENSION_RECORD = 100,
    CHECKPOINT_RECORD = 254,
}

/// Non-vanilla WAL structure (file header).
@noRecord
model WALHeader {
    databaseID: uuid
    enableChecksums: boolean
}

/// Polymorphic base for all WAL records.
@abstract
model WALRecord {
    type: WALRecordType
}

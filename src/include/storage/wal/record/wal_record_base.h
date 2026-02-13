#pragma once

#include <cstdint>
#include <memory>

#include "common/cast.h"
#include "common/copy_constructors.h"
#include "common/types/uuid.h"

namespace lbug {
namespace common {
class Serializer;
class Deserializer;
} // namespace common

namespace main {
class ClientContext;
} // namespace main

namespace storage {

enum class WALRecordType : uint8_t {
    INVALID_RECORD = 0, // This is not used for any record. 0 is reserved to detect cases where we
                        // accidentally read from an empty buffer.
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
};

struct WALHeader {
    common::ku_uuid_t databaseID;
    bool enableChecksums;
};

struct WALRecord {
    WALRecordType type = WALRecordType::INVALID_RECORD;

    WALRecord() = default;
    explicit WALRecord(WALRecordType type) : type{type} {}
    virtual ~WALRecord() = default;
    DELETE_COPY_DEFAULT_MOVE(WALRecord);

    virtual void serialize(common::Serializer& serializer) const;
    static std::unique_ptr<WALRecord> deserialize(common::Deserializer& deserializer,
        const main::ClientContext& clientContext);

    template<class TARGET>
    const TARGET& constCast() const {
        return common::ku_dynamic_cast<const TARGET&>(*this);
    }
    template<class TARGET>
    TARGET& cast() {
        return common::ku_dynamic_cast<TARGET&>(*this);
    }
};

} // namespace storage
} // namespace lbug

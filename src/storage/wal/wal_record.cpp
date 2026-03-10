#include "storage/wal/wal_record.h"

#include "common/exception/runtime.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "main/client_context.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

void WALRecord::serialize(Serializer& serializer) const {
    serializer.writeDebuggingInfo("type");
    serializer.write(type);
}

std::unique_ptr<WALRecord> WALRecord::deserialize(Deserializer& deserializer,
    const main::ClientContext& clientContext) {
    std::string key;
    auto type = WALRecordType::INVALID_RECORD;
    deserializer.getReader()->onObjectBegin();
    deserializer.validateDebuggingInfo(key, "type");
    deserializer.deserializeValue(type);
    std::unique_ptr<WALRecord> walRecord;
    switch (type) {
    case WALRecordType::BEGIN_TRANSACTION_RECORD: {
        walRecord = BeginTransactionRecord::deserialize(deserializer);
    } break;
    case WALRecordType::COMMIT_RECORD: {
        walRecord = CommitRecord::deserialize(deserializer);
    } break;
    case WALRecordType::CREATE_CATALOG_ENTRY_RECORD: {
        walRecord = CreateCatalogEntryRecord::deserialize(deserializer);
    } break;
    case WALRecordType::DROP_CATALOG_ENTRY_RECORD: {
        walRecord = DropCatalogEntryRecord::deserialize(deserializer);
    } break;
    case WALRecordType::ALTER_TABLE_ENTRY_RECORD: {
        walRecord = AlterTableEntryRecord::deserialize(deserializer);
    } break;
    case WALRecordType::TABLE_INSERTION_RECORD: {
        walRecord = TableInsertionRecord::deserialize(deserializer, clientContext);
    } break;
    case WALRecordType::NODE_DELETION_RECORD: {
        walRecord = NodeDeletionRecord::deserialize(deserializer, clientContext);
    } break;
    case WALRecordType::NODE_UPDATE_RECORD: {
        walRecord = NodeUpdateRecord::deserialize(deserializer, clientContext);
    } break;
    case WALRecordType::REL_DELETION_RECORD: {
        walRecord = RelDeletionRecord::deserialize(deserializer, clientContext);
    } break;
    case WALRecordType::REL_DETACH_DELETE_RECORD: {
        walRecord = RelDetachDeleteRecord::deserialize(deserializer, clientContext);
    } break;
    case WALRecordType::REL_UPDATE_RECORD: {
        walRecord = RelUpdateRecord::deserialize(deserializer, clientContext);
    } break;
    case WALRecordType::COPY_TABLE_RECORD: {
        walRecord = CopyTableRecord::deserialize(deserializer);
    } break;
    case WALRecordType::CHECKPOINT_RECORD: {
        walRecord = CheckpointRecord::deserialize(deserializer);
    } break;
    case WALRecordType::UPDATE_SEQUENCE_RECORD: {
        walRecord = UpdateSequenceRecord::deserialize(deserializer);
    } break;
    case WALRecordType::LOAD_EXTENSION_RECORD: {
        walRecord = LoadExtensionRecord::deserialize(deserializer);
    } break;
    case WALRecordType::INVALID_RECORD: {
        throw RuntimeException("Corrupted wal file. Read out invalid WAL record type.");
    }
    default: {
        UNREACHABLE_CODE;
    }
    }
    walRecord->type = type;
    deserializer.getReader()->onObjectEnd();
    return walRecord;
}

} // namespace storage
} // namespace lbug

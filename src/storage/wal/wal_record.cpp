#include "storage/wal/wal_record.h"

#include <algorithm>

#include "common/exception/runtime.h"
#include "common/serializer/buffer_writer.h"
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

void WALRecord::serializeWithLength(Serializer& serializer, const WALRecord& record) {
    auto bufferWriter = std::make_shared<BufferWriter>();
    Serializer bufferSerializer{bufferWriter};
    record.serialize(bufferSerializer);
    bufferSerializer.writeDebuggingInfo("ownerCatalogName");
    bufferSerializer.write<std::string>(record.ownerCatalogName);

    const auto recordLength = bufferWriter->getSize();
    serializer.write(recordLength);
    serializer.write(bufferWriter->getBlobData(), recordLength);
}

std::unique_ptr<WALRecord> WALRecord::deserialize(Deserializer& deserializer,
    const main::ClientContext& clientContext) {
    std::string key;
    auto type = WALRecordType::INVALID_RECORD;
    deserializer.getReader()->onObjectBegin();
    uint64_t recordLength = 0;
    deserializer.deserializeValue(recordLength);
    deserializer.beginReadLimit(recordLength);
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
    case WALRecordType::CREATE_INDEX_RECORD: {
        walRecord = CreateIndexRecord::deserialize(deserializer);
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
    case WALRecordType::UPDATE_SEQUENCE_NAMED_RECORD: {
        walRecord = UpdateSequenceNamedRecord::deserialize(deserializer);
    } break;
    case WALRecordType::LOAD_EXTENSION_RECORD: {
        walRecord = LoadExtensionRecord::deserialize(deserializer);
    } break;
    default: {
        throw RuntimeException("Corrupted wal file. Read out invalid WAL record type.");
    }
    }
    bool hasOwnerTrailer = deserializer.hasRemainingData();
    bool hasCompleteLengthField = false;
    uint64_t declaredOwnerNameLength = 0;
    uint64_t decodedOwnerNameLength = 0;
    bool hasTrailingInFrame = false;
    if (hasOwnerTrailer) {
        deserializer.validateDebuggingInfo(key, "ownerCatalogName");
        hasCompleteLengthField = deserializer.getRemainingReadLimit() >= sizeof(uint64_t);
        deserializer.deserializeValue(declaredOwnerNameLength);
        decodedOwnerNameLength =
            std::min(declaredOwnerNameLength, deserializer.getRemainingReadLimit());
        auto& ownerCatalogName = walRecord->ownerCatalogName;
        ownerCatalogName.resize(decodedOwnerNameLength);
        deserializer.read(reinterpret_cast<uint8_t*>(ownerCatalogName.data()),
            decodedOwnerNameLength);
        hasTrailingInFrame = deserializer.hasRemainingData();
    }
    walRecord->type = type;
    deserializer.skipReadLimit();
    deserializer.getReader()->onObjectEnd();
    if (hasOwnerTrailer) {
        if (!hasCompleteLengthField || declaredOwnerNameLength != decodedOwnerNameLength) {
            throw RuntimeException(
                "Corrupted wal file. Owner catalog name length overflows the record boundary.");
        }
        const auto& ownerCatalogName = walRecord->ownerCatalogName;
        if (ownerCatalogName.find('\0') != std::string::npos) {
            throw RuntimeException("Corrupted wal file. Owner catalog name contains a null byte.");
        }
        if (hasTrailingInFrame) {
            throw RuntimeException(
                "Corrupted wal file. Trailing bytes after the owner catalog name.");
        }
    }
    return walRecord;
}

} // namespace storage
} // namespace lbug

#include <filesystem>
#include <fstream>
#include <iostream>

#include "catalog/catalog_entry/catalog_entry.h"
#include "common/exception/storage.h"
#include "common/file_system/file_system.h"
#include "common/file_system/local_file_system.h"
#include "common/serializer/buffered_file.h"
#include "common/serializer/deserializer.h"
#include "common/types/value/value.h"
#include "main/client_context.h"
#include "main/database.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/storage_utils.h"
#include "storage/wal/checksum_reader.h"
#include "storage/wal/wal_record.h"

using namespace lbug::common;
using namespace lbug::storage;

static constexpr std::string_view checksumMismatchMessage =
    "Checksum verification failed, the WAL file is corrupted.";

static WALHeader readWALHeader(Deserializer& deserializer) {
    WALHeader header{};
    deserializer.deserializeValue(header.databaseID);
    uint8_t enableChecksumsBytes = 0;
    deserializer.deserializeValue(enableChecksumsBytes);
    header.enableChecksums = enableChecksumsBytes != 0;
    return header;
}

static std::string walRecordTypeToString(WALRecordType type) {
    switch (type) {
    case WALRecordType::BEGIN_TRANSACTION_RECORD:
        return "BEGIN_TRANSACTION_RECORD";
    case WALRecordType::COMMIT_RECORD:
        return "COMMIT_RECORD";
    case WALRecordType::COPY_TABLE_RECORD:
        return "COPY_TABLE_RECORD";
    case WALRecordType::CREATE_CATALOG_ENTRY_RECORD:
        return "CREATE_CATALOG_ENTRY_RECORD";
    case WALRecordType::DROP_CATALOG_ENTRY_RECORD:
        return "DROP_CATALOG_ENTRY_RECORD";
    case WALRecordType::ALTER_TABLE_ENTRY_RECORD:
        return "ALTER_TABLE_ENTRY_RECORD";
    case WALRecordType::UPDATE_SEQUENCE_RECORD:
        return "UPDATE_SEQUENCE_RECORD";
    case WALRecordType::TABLE_INSERTION_RECORD:
        return "TABLE_INSERTION_RECORD";
    case WALRecordType::NODE_DELETION_RECORD:
        return "NODE_DELETION_RECORD";
    case WALRecordType::NODE_UPDATE_RECORD:
        return "NODE_UPDATE_RECORD";
    case WALRecordType::REL_DELETION_RECORD:
        return "REL_DELETION_RECORD";
    case WALRecordType::REL_DETACH_DELETE_RECORD:
        return "REL_DETACH_DELETE_RECORD";
    case WALRecordType::REL_UPDATE_RECORD:
        return "REL_UPDATE_RECORD";
    case WALRecordType::LOAD_EXTENSION_RECORD:
        return "LOAD_EXTENSION_RECORD";
    case WALRecordType::CHECKPOINT_RECORD:
        return "CHECKPOINT_RECORD";
    case WALRecordType::INVALID_RECORD:
        return "INVALID_RECORD";
    default:
        return "UNKNOWN_RECORD";
    }
}

static void printValueVector(const ValueVector& vector, uint64_t numRows) {
    std::cout << "        Values: [";
    for (uint64_t i = 0; i < std::min(numRows, (uint64_t)10); i++) {
        if (i > 0)
            std::cout << ", ";
        if (vector.isNull(i)) {
            std::cout << "NULL";
        } else {
            std::cout << vector.getAsValue(i)->toString();
        }
    }
    if (numRows > 10) {
        std::cout << ", ... (" << numRows << " total)";
    }
    std::cout << "]\n";
}

static void dumpRecord(const WALRecord& record) {
    switch (record.type) {
    case WALRecordType::BEGIN_TRANSACTION_RECORD:
        std::cout << "      Type: BEGIN_TRANSACTION\n";
        break;
    case WALRecordType::COMMIT_RECORD:
        std::cout << "      Type: COMMIT\n";
        break;
    case WALRecordType::CHECKPOINT_RECORD:
        std::cout << "      Type: CHECKPOINT\n";
        break;
    case WALRecordType::COPY_TABLE_RECORD: {
        const auto& copyRecord = record.constCast<CopyTableRecord>();
        std::cout << "      Type: COPY_TABLE\n";
        std::cout << "      TableID: " << copyRecord.tableID << "\n";
        break;
    }
    case WALRecordType::CREATE_CATALOG_ENTRY_RECORD: {
        const auto& createRecord = record.constCast<CreateCatalogEntryRecord>();
        std::cout << "      Type: CREATE_CATALOG_ENTRY\n";
        if (createRecord.ownedCatalogEntry) {
            std::cout << "      Entry Type: "
                      << static_cast<uint8_t>(createRecord.ownedCatalogEntry->getType()) << "\n";
            std::cout << "      Entry Name: " << createRecord.ownedCatalogEntry->getName() << "\n";
        }
        std::cout << "      IsInternal: " << (createRecord.isInternal ? "true" : "false") << "\n";
        break;
    }
    case WALRecordType::DROP_CATALOG_ENTRY_RECORD: {
        const auto& dropRecord = record.constCast<DropCatalogEntryRecord>();
        std::cout << "      Type: DROP_CATALOG_ENTRY\n";
        std::cout << "      EntryID: " << dropRecord.entryID << "\n";
        std::cout << "      EntryType: " << static_cast<uint8_t>(dropRecord.entryType) << "\n";
        break;
    }
    case WALRecordType::ALTER_TABLE_ENTRY_RECORD: {
        const auto& alterRecord = record.constCast<AlterTableEntryRecord>();
        std::cout << "      Type: ALTER_TABLE_ENTRY\n";
        if (alterRecord.ownedAlterInfo) {
            std::cout << "      TableName: " << alterRecord.ownedAlterInfo->tableName << "\n";
            std::cout << "      AlterType: "
                      << static_cast<uint8_t>(alterRecord.ownedAlterInfo->alterType) << "\n";
        }
        break;
    }
    case WALRecordType::UPDATE_SEQUENCE_RECORD: {
        const auto& seqRecord = record.constCast<UpdateSequenceRecord>();
        std::cout << "      Type: UPDATE_SEQUENCE\n";
        std::cout << "      SequenceID: " << seqRecord.sequenceID << "\n";
        std::cout << "      KCount: " << seqRecord.kCount << "\n";
        break;
    }
    case WALRecordType::LOAD_EXTENSION_RECORD: {
        const auto& extRecord = record.constCast<LoadExtensionRecord>();
        std::cout << "      Type: LOAD_EXTENSION\n";
        std::cout << "      Path: " << extRecord.path << "\n";
        break;
    }
    case WALRecordType::TABLE_INSERTION_RECORD: {
        const auto& insertRecord = record.constCast<TableInsertionRecord>();
        std::cout << "      Type: TABLE_INSERTION\n";
        std::cout << "      TableID: " << insertRecord.tableID << "\n";
        std::cout << "      TableType: " << static_cast<uint8_t>(insertRecord.tableType) << "\n";
        std::cout << "      NumRows: " << insertRecord.numRows << "\n";
        std::cout << "      NumVectors: " << insertRecord.ownedVectors.size() << "\n";
        for (size_t i = 0; i < insertRecord.ownedVectors.size(); i++) {
            std::cout << "      Vector " << i << ":\n";
            printValueVector(*insertRecord.ownedVectors[i], insertRecord.numRows);
        }
        break;
    }
    case WALRecordType::NODE_DELETION_RECORD: {
        const auto& deleteRecord = record.constCast<NodeDeletionRecord>();
        std::cout << "      Type: NODE_DELETION\n";
        std::cout << "      TableID: " << deleteRecord.tableID << "\n";
        std::cout << "      NodeOffset: " << deleteRecord.nodeOffset << "\n";
        if (deleteRecord.ownedPKVector) {
            std::cout << "      PK Value: " << deleteRecord.ownedPKVector->getAsValue(0)->toString()
                      << "\n";
        }
        break;
    }
    case WALRecordType::NODE_UPDATE_RECORD: {
        const auto& updateRecord = record.constCast<NodeUpdateRecord>();
        std::cout << "      Type: NODE_UPDATE\n";
        std::cout << "      TableID: " << updateRecord.tableID << "\n";
        std::cout << "      ColumnID: " << updateRecord.columnID << "\n";
        std::cout << "      NodeOffset: " << updateRecord.nodeOffset << "\n";
        if (updateRecord.ownedPropertyVector) {
            std::cout << "      PropertyValue: "
                      << updateRecord.ownedPropertyVector->getAsValue(0)->toString() << "\n";
        }
        break;
    }
    case WALRecordType::REL_DELETION_RECORD: {
        const auto& deleteRecord = record.constCast<RelDeletionRecord>();
        std::cout << "      Type: REL_DELETION\n";
        std::cout << "      TableID: " << deleteRecord.tableID << "\n";
        if (deleteRecord.ownedSrcNodeIDVector && !deleteRecord.ownedSrcNodeIDVector->isNull(0)) {
            auto srcNode = deleteRecord.ownedSrcNodeIDVector->getValue<nodeID_t>(0);
            std::cout << "      SrcNode: (table:" << srcNode.tableID
                      << ", offset:" << srcNode.offset << ")\n";
        }
        if (deleteRecord.ownedDstNodeIDVector && !deleteRecord.ownedDstNodeIDVector->isNull(0)) {
            auto dstNode = deleteRecord.ownedDstNodeIDVector->getValue<nodeID_t>(0);
            std::cout << "      DstNode: (table:" << dstNode.tableID
                      << ", offset:" << dstNode.offset << ")\n";
        }
        if (deleteRecord.ownedRelIDVector && !deleteRecord.ownedRelIDVector->isNull(0)) {
            auto relID = deleteRecord.ownedRelIDVector->getValue<internalID_t>(0);
            std::cout << "      RelID: (table:" << relID.tableID << ", offset:" << relID.offset
                      << ")\n";
        }
        break;
    }
    case WALRecordType::REL_DETACH_DELETE_RECORD: {
        const auto& detachRecord = record.constCast<RelDetachDeleteRecord>();
        std::cout << "      Type: REL_DETACH_DELETE\n";
        std::cout << "      TableID: " << detachRecord.tableID << "\n";
        std::cout << "      Direction: " << static_cast<uint8_t>(detachRecord.direction) << "\n";
        if (detachRecord.ownedSrcNodeIDVector) {
            printValueVector(*detachRecord.ownedSrcNodeIDVector, 1);
        }
        break;
    }
    case WALRecordType::REL_UPDATE_RECORD: {
        const auto& updateRecord = record.constCast<RelUpdateRecord>();
        std::cout << "      Type: REL_UPDATE\n";
        std::cout << "      TableID: " << updateRecord.tableID << "\n";
        std::cout << "      ColumnID: " << updateRecord.columnID << "\n";
        if (updateRecord.ownedPropertyVector) {
            std::cout << "      PropertyValue: "
                      << updateRecord.ownedPropertyVector->getAsValue(0)->toString() << "\n";
        }
        break;
    }
    default:
        std::cout << "      Type: UNKNOWN (" << static_cast<uint8_t>(record.type) << ")\n";
        break;
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <database_path>\n";
        return 1;
    }

    std::string databasePath = argv[1];
    std::string walPath = StorageUtils::getWALFilePath(databasePath);

    std::cout << "WAL File: " << walPath << "\n\n";

    if (!std::filesystem::exists(walPath)) {
        std::cout << "WAL file does not exist. Database was cleanly shutdown or no modifications "
                     "were made.\n";
        return 0;
    }

    try {
        LocalFileSystem lfs("");
        auto fileInfo = lfs.openFile(walPath, FileOpenFlags(FileFlags::READ_ONLY));
        auto fileSize = fileInfo->getFileSize();
        if (fileSize == 0) {
            std::cout << "WAL file is empty. Database was cleanly shutdown.\n";
            return 0;
        }

        // Create an isolated in-memory client context for record deserialization.
        auto contextDB = std::make_unique<lbug::main::Database>(":memory:");
        lbug::main::ClientContext clientContext(contextDB.get());

        std::unique_ptr<Reader> reader;
        if (fileSize >= sizeof(ku_uuid_t) + sizeof(uint8_t) + sizeof(uint64_t)) {
            reader = std::make_unique<ChecksumReader>(*fileInfo, *MemoryManager::Get(clientContext),
                checksumMismatchMessage);
        } else {
            reader = std::make_unique<BufferedFileReader>(*fileInfo);
        }
        Deserializer deserializer(std::move(reader));

        deserializer.getReader()->onObjectBegin();
        auto walHeader = readWALHeader(deserializer);
        deserializer.getReader()->onObjectEnd();

        std::cout << "WAL Header:\n";
        std::cout << "  Database ID: " << UUID::toString(walHeader.databaseID) << "\n";
        std::cout << "  Checksums Enabled: " << (walHeader.enableChecksums ? "true" : "false")
                  << "\n";
        std::cout << "  File Size: " << fileSize << " bytes\n\n";

        std::cout << "Record offsets:\n";

        uint64_t recordCount = 0;
        uint64_t lastOffset = 0;

        while (!deserializer.finished()) {
            lastOffset = walHeader.enableChecksums ?
                             deserializer.getReader()->cast<ChecksumReader>()->getReadOffset() :
                             deserializer.getReader()->cast<BufferedFileReader>()->getReadOffset();
            auto walRecord = WALRecord::deserialize(deserializer, clientContext);
            std::cout << "  Record at offset " << lastOffset << " ("
                      << walRecordTypeToString(walRecord->type) << "):\n";
            dumpRecord(*walRecord);
            recordCount++;
        }

        std::cout << "\nTotal records found: " << recordCount << "\n";
        std::cout << "Last offset: " << lastOffset << "\n";

    } catch (const StorageException&) {
        std::cerr << "Error: WAL file is corrupted - checksum verification failed.\n";
        std::cerr << "This WAL file cannot be read.\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error reading WAL file: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

#include "storage/wal/local_wal.h"

#include "binder/ddl/bound_alter_info.h"
#include "catalog/catalog_entry/sequence_catalog_entry.h"
#include "common/serializer/in_mem_file_writer.h"
#include "common/vector/value_vector.h"
#include "storage/wal/checksum_writer.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::binder;

namespace lbug {
namespace storage {

LocalWAL::LocalWAL(MemoryManager& mm, bool enableChecksums)
    : inMemWriter(std::make_shared<InMemFileWriter>(mm)),
      serializer(enableChecksums ? std::make_shared<ChecksumWriter>(inMemWriter, mm) :
                                   std::static_pointer_cast<Writer>(inMemWriter)) {}

void LocalWAL::logCommit() {
    std::unique_lock lck{mtx};
    if (!hasLoggedBegin) {
        return;
    }
    CommitRecord walRecord;
    addNewWALRecordNoLock(walRecord);
}

void LocalWAL::logCreateCatalogEntryRecord(const std::string& ownerCatalogName,
    CatalogEntry* catalogEntry, bool isInternal) {
    CreateCatalogEntryRecord walRecord(catalogEntry, isInternal);
    walRecord.ownerCatalogName = ownerCatalogName;
    addNewWALRecord(walRecord);
}

void LocalWAL::logCreateIndexRecord(const std::string& ownerCatalogName, CatalogEntry* catalogEntry,
    IndexInfo indexInfo, std::vector<uint8_t> treeBytes) {
    CreateIndexRecord walRecord(catalogEntry, std::move(indexInfo), std::move(treeBytes));
    walRecord.ownerCatalogName = ownerCatalogName;
    addNewWALRecord(walRecord);
}

void LocalWAL::logDropCatalogEntryRecord(const std::string& ownerCatalogName, table_id_t tableID,
    CatalogEntryType type) {
    DropCatalogEntryRecord walRecord(tableID, type);
    walRecord.ownerCatalogName = ownerCatalogName;
    addNewWALRecord(walRecord);
}

void LocalWAL::logAlterCatalogEntryRecord(const std::string& ownerCatalogName,
    const BoundAlterInfo* alterInfo) {
    AlterTableEntryRecord walRecord(alterInfo);
    walRecord.ownerCatalogName = ownerCatalogName;
    addNewWALRecord(walRecord);
}

void LocalWAL::logTableInsertion(const std::string& ownerCatalogName, table_id_t tableID,
    TableType tableType, row_idx_t numRows, const std::vector<ValueVector*>& vectors) {
    TableInsertionRecord walRecord(tableID, tableType, numRows, vectors);
    walRecord.ownerCatalogName = ownerCatalogName;
    addNewWALRecord(walRecord);
}

void LocalWAL::logNodeDeletion(const std::string& ownerCatalogName, table_id_t tableID,
    offset_t nodeOffset, ValueVector* pkVector) {
    NodeDeletionRecord walRecord(tableID, nodeOffset, pkVector);
    walRecord.ownerCatalogName = ownerCatalogName;
    addNewWALRecord(walRecord);
}

void LocalWAL::logNodeUpdate(const std::string& ownerCatalogName, table_id_t tableID,
    column_id_t columnID, offset_t nodeOffset, ValueVector* propertyVector) {
    NodeUpdateRecord walRecord(tableID, columnID, nodeOffset, propertyVector);
    walRecord.ownerCatalogName = ownerCatalogName;
    addNewWALRecord(walRecord);
}

void LocalWAL::logRelDelete(const std::string& ownerCatalogName, table_id_t tableID,
    ValueVector* srcNodeVector, ValueVector* dstNodeVector, ValueVector* relIDVector) {
    RelDeletionRecord walRecord(tableID, srcNodeVector, dstNodeVector, relIDVector);
    walRecord.ownerCatalogName = ownerCatalogName;
    addNewWALRecord(walRecord);
}

void LocalWAL::logRelDetachDelete(const std::string& ownerCatalogName, table_id_t tableID,
    RelDataDirection direction, ValueVector* srcNodeVector) {
    RelDetachDeleteRecord walRecord(tableID, direction, srcNodeVector);
    walRecord.ownerCatalogName = ownerCatalogName;
    addNewWALRecord(walRecord);
}

void LocalWAL::logRelUpdate(const std::string& ownerCatalogName, table_id_t tableID,
    column_id_t columnID, ValueVector* srcNodeVector, ValueVector* dstNodeVector,
    ValueVector* relIDVector, ValueVector* propertyVector) {
    RelUpdateRecord walRecord(tableID, columnID, srcNodeVector, dstNodeVector, relIDVector,
        propertyVector);
    walRecord.ownerCatalogName = ownerCatalogName;
    addNewWALRecord(walRecord);
}

void LocalWAL::logUpdateSequenceRecord(const std::string& ownerCatalogName,
    sequence_id_t sequenceID, uint64_t kCount, const std::string& sequenceName) {
    UpdateSequenceRecord walRecord(sequenceID, kCount, sequenceName);
    walRecord.ownerCatalogName = ownerCatalogName;
    addNewWALRecord(walRecord);
}

void LocalWAL::logLoadExtension(const std::string& ownerCatalogName, std::string path) {
    LoadExtensionRecord walRecord(std::move(path));
    walRecord.ownerCatalogName = ownerCatalogName;
    addNewWALRecord(walRecord);
}

// NOLINTNEXTLINE(readability-make-member-function-const): semantically non-const function.
void LocalWAL::clear() {
    std::unique_lock lck{mtx};
    serializer.getWriter()->clear();
    hasLoggedBegin = false;
}

uint64_t LocalWAL::getSize() {
    std::unique_lock lck{mtx};
    return serializer.getWriter()->getSize();
}

// NOLINTNEXTLINE(readability-make-member-function-const): semantically non-const function.
void LocalWAL::addNewWALRecord(const WALRecord& walRecord) {
    std::unique_lock lck{mtx};
    if (!hasLoggedBegin && walRecord.type != WALRecordType::BEGIN_TRANSACTION_RECORD &&
        walRecord.type != WALRecordType::COMMIT_RECORD) {
        BeginTransactionRecord beginRecord;
        addNewWALRecordNoLock(beginRecord);
        hasLoggedBegin = true;
    }
    addNewWALRecordNoLock(walRecord);
}

// NOLINTNEXTLINE(readability-make-member-function-const): semantically non-const function.
void LocalWAL::addNewWALRecordNoLock(const WALRecord& walRecord) {
    DASSERT(walRecord.type != WALRecordType::INVALID_RECORD);
    serializer.getWriter()->onObjectBegin();
    WALRecord::serializeWithLength(serializer, walRecord);
    serializer.getWriter()->onObjectEnd();
}

} // namespace storage
} // namespace lbug

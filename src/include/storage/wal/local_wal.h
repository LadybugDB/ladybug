#pragma once

#include <mutex>

#include "storage/wal/wal_record.h"

namespace lbug {
namespace binder {
struct BoundAlterInfo;
} // namespace binder
namespace common {
class InMemFileWriter;
class ValueVector;
} // namespace common
namespace catalog {
class CatalogEntry;
} // namespace catalog

namespace storage {
class WAL;
class LocalWAL {
    friend class WAL;

public:
    explicit LocalWAL(MemoryManager& mm, bool enableChecksums);

    void logCreateCatalogEntryRecord(const std::string& ownerCatalogName,
        catalog::CatalogEntry* catalogEntry, bool isInternal);
    void logCreateIndexRecord(const std::string& ownerCatalogName,
        catalog::CatalogEntry* catalogEntry, IndexInfo indexInfo, std::vector<uint8_t> treeBytes);
    void logDropCatalogEntryRecord(const std::string& ownerCatalogName, uint64_t tableID,
        catalog::CatalogEntryType type);
    void logAlterCatalogEntryRecord(const std::string& ownerCatalogName,
        const binder::BoundAlterInfo* alterInfo);
    void logUpdateSequenceRecord(const std::string& ownerCatalogName,
        common::sequence_id_t sequenceID, uint64_t kCount);

    void logTableInsertion(const std::string& ownerCatalogName, common::table_id_t tableID,
        common::TableType tableType, common::row_idx_t numRows,
        const std::vector<common::ValueVector*>& vectors);
    void logNodeDeletion(const std::string& ownerCatalogName, common::table_id_t tableID,
        common::offset_t nodeOffset, common::ValueVector* pkVector);
    void logNodeUpdate(const std::string& ownerCatalogName, common::table_id_t tableID,
        common::column_id_t columnID, common::offset_t nodeOffset,
        common::ValueVector* propertyVector);
    void logRelDelete(const std::string& ownerCatalogName, common::table_id_t tableID,
        common::ValueVector* srcNodeVector, common::ValueVector* dstNodeVector,
        common::ValueVector* relIDVector);
    void logRelDetachDelete(const std::string& ownerCatalogName, common::table_id_t tableID,
        common::RelDataDirection direction, common::ValueVector* srcNodeVector);
    void logRelUpdate(const std::string& ownerCatalogName, common::table_id_t tableID,
        common::column_id_t columnID, common::ValueVector* srcNodeVector,
        common::ValueVector* dstNodeVector, common::ValueVector* relIDVector,
        common::ValueVector* propertyVector);

    void logLoadExtension(const std::string& ownerCatalogName, std::string path);

    void logCommit();

    void clear();
    uint64_t getSize();

private:
    void addNewWALRecord(const WALRecord& walRecord);
    void addNewWALRecordNoLock(const WALRecord& walRecord);

private:
    std::mutex mtx;
    std::shared_ptr<common::InMemFileWriter> inMemWriter;
    common::Serializer serializer;
    bool hasLoggedBegin = false;
};

} // namespace storage
} // namespace lbug

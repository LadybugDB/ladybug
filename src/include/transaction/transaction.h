#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

#include "common/types/types.h"

namespace lbug {
namespace binder {
struct BoundAlterInfo;
}
namespace catalog {
class Catalog;
class CatalogEntry;
class CatalogSet;
class SequenceCatalogEntry;
struct SequenceRollbackData;
} // namespace catalog
namespace main {
class ClientContext;
} // namespace main
namespace storage {
class Table;
class LocalWAL;
class LocalStorage;
class UndoBuffer;
class WAL;
class VersionInfo;
class UpdateInfo;
struct VectorUpdateInfo;
class ChunkedNodeGroup;
class VersionRecordHandler;
} // namespace storage
namespace transaction {
class TransactionManager;

enum class TransactionType : uint8_t { READ_ONLY, WRITE, CHECKPOINT, DUMMY, RECOVERY };

class LocalCacheManager;
class LBUG_API LocalCacheObject {
public:
    explicit LocalCacheObject(std::string key) : key{std::move(key)} {}

    virtual ~LocalCacheObject() = default;

    std::string getKey() const { return key; }

    template<typename T>
    T* cast() {
        return common::dynamic_cast_checked<T*>(this);
    }

private:
    std::string key;
};

class LocalCacheManager {
public:
    bool contains(const std::string& key) {
        std::unique_lock lck{mtx};
        return cachedObjects.contains(key);
    }
    LocalCacheObject& at(const std::string& key) {
        std::unique_lock lck{mtx};
        return *cachedObjects.at(key);
    }
    bool put(std::unique_ptr<LocalCacheObject> object);

    void remove(const std::string& key) {
        std::unique_lock lck{mtx};
        cachedObjects.erase(key);
    }

private:
    std::unordered_map<std::string, std::unique_ptr<LocalCacheObject>> cachedObjects;
    std::mutex mtx;
};

class LBUG_API Transaction {
    friend class TransactionManager;

public:
    static constexpr common::transaction_t DUMMY_TRANSACTION_ID = 0;
    static constexpr common::transaction_t DUMMY_START_TIMESTAMP = 0;
    static constexpr common::transaction_t START_TRANSACTION_ID =
        static_cast<common::transaction_t>(1) << 63;

    Transaction(main::ClientContext& clientContext, TransactionType transactionType,
        common::transaction_t transactionID, common::transaction_t startTS);

    explicit Transaction(TransactionType transactionType) noexcept;
    Transaction(TransactionType transactionType, common::transaction_t ID,
        common::transaction_t startTS) noexcept;

    ~Transaction();

    TransactionType getType() const { return type; }
    bool isReadOnly() const { return TransactionType::READ_ONLY == type; }
    bool isWriteTransaction() const { return TransactionType::WRITE == type; }
    bool isDummy() const { return TransactionType::DUMMY == type; }
    bool isRecovery() const { return TransactionType::RECOVERY == type; }
    common::transaction_t getID() const { return ID; }
    common::transaction_t getStartTS() const { return startTS; }
    common::transaction_t getCommitTS() const { return commitTS; }
    int64_t getCurrentTS() const { return currentTS; }

    void setForceCheckpoint() { forceCheckpoint = true; }
    bool shouldAppendToUndoBuffer() const {
        // Only write transactions and recovery transactions should append to the undo buffer.
        return isWriteTransaction() || isRecovery();
    }
    bool shouldLogToWAL() const;
    storage::LocalWAL& getLocalWAL() const {
        DASSERT(localWAL);
        return *localWAL;
    }

    bool shouldForceCheckpoint() const;

    void commit(storage::WAL* wal);
    void writeCommitToWAL(storage::WAL* wal, uint64_t& walCommitSequence);
    void publishCommit();
    void rollback(storage::WAL* wal);
    void pushCommitCallback(std::function<void(Transaction&)> callback);
    void pushRollbackCallback(std::function<void(Transaction&)> callback);

    storage::LocalStorage* getLocalStorage() const { return localStorage.get(); }
    LocalCacheManager& getLocalCacheManager() { return localCacheManager; }
    bool isUnCommitted(const storage::Table& table, common::offset_t nodeOffset) const;
    bool isUnCommitted(common::table_id_t tableID, common::offset_t nodeOffset) const;
    common::row_idx_t getLocalRowIdx(const storage::Table& table,
        common::offset_t nodeOffset) const {
        return nodeOffset - getMinUncommittedNodeOffset(table);
    }
    common::row_idx_t getLocalRowIdx(common::table_id_t tableID,
        common::offset_t nodeOffset) const {
        return nodeOffset - getMinUncommittedNodeOffset(tableID);
    }
    std::optional<common::row_idx_t> tryGetLocalRowIdx(const storage::Table& table,
        common::offset_t nodeOffset) const;
    common::offset_t getUncommittedOffset(const storage::Table& table,
        common::row_idx_t localRowIdx) const {
        return getMinUncommittedNodeOffset(table) + localRowIdx;
    }

    main::ClientContext* getClientContext() const { return clientContext; }

    void pushCreateDropCatalogEntry(catalog::CatalogSet& catalogSet,
        catalog::CatalogEntry& catalogEntry, bool isInternal, bool skipLoggingToWAL = false);
    void pushAlterCatalogEntry(catalog::CatalogSet& catalogSet, catalog::CatalogEntry& catalogEntry,
        const binder::BoundAlterInfo& alterInfo, bool skipLoggingToWAL = false,
        common::table_id_t addedRelTableOID = common::INVALID_TABLE_ID);
    void pushSequenceChange(catalog::SequenceCatalogEntry* sequenceEntry, int64_t kCount,
        const catalog::SequenceRollbackData& data);
    // The transaction's undo records may point into the parked catalog; destroying it
    // before commit or rollback finishes would leave those records dangling.
    void retireGraphCatalog(std::unique_ptr<catalog::Catalog> catalog);
    void pushInsertInfo(common::node_group_idx_t nodeGroupIdx, common::row_idx_t startRow,
        common::row_idx_t numRows, const storage::VersionRecordHandler* versionRecordHandler) const;
    void pushDeleteInfo(common::node_group_idx_t nodeGroupIdx, common::row_idx_t startRow,
        common::row_idx_t numRows, const storage::VersionRecordHandler* versionRecordHandler) const;
    void pushVectorUpdateInfo(storage::UpdateInfo& updateInfo, common::idx_t vectorIdx,
        storage::VectorUpdateInfo& vectorUpdateInfo, common::transaction_t version) const;

    static Transaction* Get(const main::ClientContext& context);

private:
    common::offset_t getMinUncommittedNodeOffset(const storage::Table& table) const;
    common::offset_t getMinUncommittedNodeOffset(common::table_id_t tableID) const;
    void recordCatalogChange(catalog::Catalog* catalog);

private:
    TransactionType type;
    common::transaction_t ID;
    common::transaction_t startTS;
    common::transaction_t commitTS;
    int64_t currentTS;
    main::ClientContext* clientContext;
    std::unique_ptr<storage::LocalStorage> localStorage;
    std::unique_ptr<storage::UndoBuffer> undoBuffer;
    std::unique_ptr<storage::LocalWAL> localWAL;
    LocalCacheManager localCacheManager;
    std::vector<std::function<void(Transaction&)>> commitCallbacks;
    std::vector<std::function<void(Transaction&)>> rollbackCallbacks;
    bool forceCheckpoint;
    std::vector<std::unique_ptr<catalog::Catalog>> retiredGraphCatalogs;
    std::unordered_set<catalog::Catalog*> changedCatalogs;
    std::mutex changedCatalogsMutex;
};

// TODO(bmwinger): These shouldn't need to be exported
extern LBUG_API Transaction DUMMY_TRANSACTION;
extern LBUG_API Transaction DUMMY_CHECKPOINT_TRANSACTION;

} // namespace transaction
} // namespace lbug

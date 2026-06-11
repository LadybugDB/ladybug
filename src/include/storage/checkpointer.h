#pragma once

#include <unordered_map>
#include <vector>

#include "common/types/types.h"
#include "storage/database_header.h"
#include "storage/page_range.h"

namespace lbug {
namespace transaction {
class Transaction;
}
namespace catalog {
class Catalog;
}
namespace common {
class VirtualFileSystem;
} // namespace common
namespace testing {
struct FSMLeakChecker;
}
namespace main {
class AttachedLbugDatabase;
class DatabaseManager;
} // namespace main

namespace storage {
class StorageManager;

class Checkpointer {
    friend class main::AttachedLbugDatabase;
    friend class main::DatabaseManager;
    friend struct testing::FSMLeakChecker;

public:
    explicit Checkpointer(main::ClientContext& clientContext);
    virtual ~Checkpointer();

    void writeCheckpoint();
    void beginCheckpoint(common::transaction_t snapshotTS);
    // Storage materialization phase. Safe to call after the write gate is released when WAL
    // rotation occurred — node-data reads use the frozen WAL bounded to snapshotTS.
    // See transaction_manager.cpp for the hash-index timestamp caveat.
    void checkpointStoragePhase();
    void finishCheckpoint();
    // Cleanup after the core checkpoint. If the write gate is held, page-manager changes made by
    // the checkpoint itself can be marked clean.
    void postCheckpointCleanup(bool canResetPageManagerToCurrent = true);
    void rollback();
    bool wasWalRotated() const { return walRotated; }

    void readCheckpoint();

    static bool canAutoCheckpoint(const main::ClientContext& clientContext,
        const transaction::Transaction& transaction);

protected:
    virtual bool checkpointStorage();
    virtual void serializeCatalogAndMetadata(DatabaseHeader& databaseHeader,
        bool hasStorageChanges);
    virtual void writeDatabaseHeader(const DatabaseHeader& header);
    virtual void logCheckpointAndApplyShadowPages(bool walRotated = false);

private:
    struct CheckpointTarget {
        catalog::Catalog* catalog;
        StorageManager* storageManager;
    };

    std::vector<CheckpointTarget> collectCheckpointTargets() const;

    static void readCheckpoint(main::ClientContext* context, catalog::Catalog* catalog,
        StorageManager* storageManager);

    PageRange serializeCatalog(const catalog::Catalog& catalog, StorageManager& storageManager);
    PageRange serializeCatalogSnapshot(const catalog::Catalog& catalog,
        StorageManager& storageManager);
    PageRange serializeMetadata(const catalog::Catalog& catalog, StorageManager& storageManager);
    PageRange serializeMetadataSnapshot(const catalog::Catalog& catalog,
        StorageManager& storageManager);

protected:
    main::ClientContext& clientContext;
    bool isInMemory;
    StorageManager* mainStorageManager;
    bool walRotated = false;
    // Snapshot timestamp captured at drain time for MVCC catalog serialization.
    common::transaction_t snapshotTS = 0;
    // Database header captured during beginCheckpoint for use in finishCheckpoint.
    DatabaseHeader checkpointHeader{};
    // Whether storage had changes during checkpointStoragePhase.
    bool hasStorageChanges = false;
    // Whether this checkpoint upgrades the durable catalog/header storage layout.
    bool hasStorageVersionUpgrade = false;
    // Versions captured at the end of writeCheckpoint() while the write gate is still held.
    uint64_t catalogVersionAtCheckpoint = 0;
    uint64_t pageManagerVersionAtCheckpoint = 0;
    // Per-table changeEpoch watermarks captured under the write gate.
    std::unordered_map<common::table_id_t, uint64_t> tableEpochWatermarks;
    std::unordered_map<StorageManager*, bool> storageChangesByManager;
    std::unordered_map<StorageManager*, bool> walRotatedByManager;
    std::vector<CheckpointTarget> checkpointTargets;
    std::unordered_map<StorageManager*, std::unordered_map<common::table_id_t, uint64_t>>
        tableEpochWatermarksByManager;
    std::unordered_map<catalog::Catalog*, uint64_t> catalogVersionAtCheckpointByCatalog;
    std::unordered_map<StorageManager*, uint64_t> pageManagerVersionAtCheckpointByManager;
};

} // namespace storage
} // namespace lbug

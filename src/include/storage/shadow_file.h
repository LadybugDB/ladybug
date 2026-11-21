#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "common/copy_constructors.h"
#include "common/types/uuid.h"
#include "storage/file_handle.h"

namespace lbug {
namespace storage {

class BufferManager;
class StorageManager;

struct ShadowPageRecord {
    common::file_idx_t originalFileIdx = common::INVALID_PAGE_IDX;
    common::page_idx_t originalPageIdx = common::INVALID_PAGE_IDX;

    void serialize(common::Serializer& serializer) const;
    static ShadowPageRecord deserialize(common::Deserializer& deserializer);
};

struct ShadowFileHeader {
    common::uuid databaseID{0};
    common::page_idx_t numShadowPages = 0;
    // The database that owns the pending checkpoint bundle this shadow file belongs to. A shadow
    // flushed while another database runs the checkpoint (a graph's or partition child's shadow
    // during a parent checkpoint) carries that parent's ID, so a standalone open of the child file
    // can refuse to decide the bundle's fate instead of discarding its shadow. Zero means the
    // header predates bundle ownership stamping and is treated as the file's own bundle.
    common::uuid ownerDatabaseID{0};
};
static_assert(std::is_trivially_copyable_v<ShadowFileHeader>);

class BufferManager;
// NOTE: Only the checkpointing thread may create, publish or clear shadow pages. Read
// transactions running concurrently with a checkpoint may only use readShadowVersionIfExists.
class ShadowFile {
public:
    static constexpr common::uuid CHECKPOINT_BUNDLE_DATABASE_ID{
        common::int128_t{UINT64_MAX, INT64_MAX}};

    ShadowFile(BufferManager& bm, common::VirtualFileSystem* vfs, const std::string& databasePath);

    // Mutex protocol: hasShadowPage, getShadowPage, clearShadowPage, createShadowPage and
    // publishShadowPage are for the checkpointing thread only. It is the only writer of
    // shadowPagesMap, so it may read the map without taking mtx; the functions that modify the
    // map take mtx exclusively. Read transactions running concurrently with a checkpoint must not
    // call these and must use readShadowVersionIfExists, which takes mtx shared.
    // TODO(Guodong): Remove originalFile param.
    bool hasShadowPage(common::file_idx_t originalFile, common::page_idx_t originalPage) const {
        return shadowPagesMap.contains(originalFile) &&
               shadowPagesMap.at(originalFile).contains(originalPage);
    }
    void clearShadowPage(common::file_idx_t originalFile, common::page_idx_t originalPage);
    common::page_idx_t getShadowPage(common::file_idx_t originalFile,
        common::page_idx_t originalPage) const;
    // Adds a new shadow page for originalPage. The page is not visible to
    // readShadowVersionIfExists (nor hasShadowPage) until publishShadowPage is called, so the
    // caller can fill it with the contents of the original page first. Must not be called while
    // a published shadow page of a column data page is pinned (see mtx).
    common::page_idx_t createShadowPage(common::file_idx_t originalFile,
        common::page_idx_t originalPage);
    void publishShadowPage(common::file_idx_t originalFile, common::page_idx_t originalPage,
        common::page_idx_t shadowPageIdx);
    // A savepoint captures how many shadow pages have been created so far. The checkpointing
    // thread can roll back to it to drop exactly the shadow pages created since, without
    // disturbing shadow pages created by already-checkpointed tables. Used to undo a
    // partially completed node group checkpoint (see LadybugDB/ladybug#1051).
    // While a savepoint is active, the first rewrite of a shadow page that predates it saves the
    // page's contents (see preservePageForSavepoint), so rollback can restore it. Such pages come
    // from tables or groups checkpointed earlier, or from a previous failed checkpoint whose
    // shadow pages are kept for the next one.
    using ShadowSavepoint = size_t;
    ShadowSavepoint createSavepoint();
    // Ends the active savepoint without rolling back and drops the saved page contents.
    void releaseSavepoint();
    // Restores the saved contents of shadow pages that predate the savepoint, then drops all
    // shadow pages created after it: their map entries, their records, and their frames
    // (evicted without flushing, so their contents never reach the data file). Later
    // allocations reuse the freed shadow page indices, preserving the invariant that
    // shadowPageRecords[i] describes shadow page i+1 (see flushAll). Ends the savepoint. Must
    // not be called while a published shadow page of a column data page is pinned (see mtx).
    void rollbackToSavepoint(ShadowSavepoint savepoint);
    // Set once a rollbackToSavepoint failed to restore a page: this shadow file may then hold a
    // failed group's bytes in a page that completed groups also own, under that group's restored
    // chunk metadata. Until the database is reopened, reads of that group can return those
    // bytes. Checkpointer::rollback poisons the WAL with this message, so no later checkpoint
    // applies the pages and no write builds on them; reopening discards the uncommitted shadow
    // file. A later message (e.g. the node group's combined error) replaces an earlier one.
    void recordSavepointRollbackFailure(std::string message) {
        savepointRollbackFailure = std::move(message);
    }
    const std::optional<std::string>& getSavepointRollbackFailure() const {
        return savepointRollbackFailure;
    }
    bool hasFailedSavepointRollback() const { return savepointRollbackFailure.has_value(); }
    // Called by the checkpointing thread with an existing shadow page pinned for writing, before
    // it is modified. Saves the page's contents once if it predates the active savepoint.
    void preservePageForSavepoint(common::page_idx_t shadowPageIdx, const uint8_t* frame);
    // Reads the shadow version of a page if the current checkpoint has one, and returns false
    // without calling readOp otherwise. Unlike the functions above, this is safe to call from
    // read transactions running concurrently with a checkpoint: a checkpoint publishes the new
    // metadata of in-place updated column chunks before the shadow pages are applied to the data
    // file, so those readers must see the shadow version of such pages.
    bool readShadowVersionIfExists(common::file_idx_t originalFile, common::page_idx_t originalPage,
        const std::function<void(uint8_t*)>& readOp) const;

    FileHandle& getShadowingFH() const { return *shadowingFH; }

    // Writes the shadow pages into the data file. `publish` runs before the apply lock is
    // released, so in-memory state that addresses the new pages (the primary-key index read
    // headers) changes together with the pages for readers holding lockForPairedRead().
    void applyShadowPages(StorageManager& storageManager, main::ClientContext& context,
        const std::function<void()>& publish = {}) const;

    // Read transactions that pair cached in-memory metadata with data-file pages (primary-key
    // lookups, #1052) hold this for the whole lookup. It excludes applyShadowPages() and its
    // publication step, so a lookup sees either the old pages and headers or the new ones,
    // never new slot pages read through old headers. Throws after a failed apply.
    //
    // Re-entrant per thread: a scope nested in another scope of the same shadow file takes no
    // second lock, so a batch of lookups can hold one scope around all of them. A null
    // shadow file makes the scope a no-op.
    class PairedReadScope {
    public:
        explicit PairedReadScope(const ShadowFile* shadowFile);
        ~PairedReadScope();
        DELETE_COPY_AND_MOVE(PairedReadScope);

    private:
        const ShadowFile* previous = nullptr;
        bool owns = false;
        std::shared_lock<std::shared_mutex> lock;
    };

    // Throws if this thread is inside applyShadowPages()'s publication step. Primary-key lookups
    // call it: a lookup there would see half-published state (CHECKPOINT transactions skip the
    // paired-read lock), and any other transaction would deadlock on it.
    static void checkNotPublishingOnThisThread();

    // `ownerDatabaseID` is the ID of the database running the checkpoint, stamped into the shadow
    // header (see ShadowFileHeader::ownerDatabaseID).
    void flushAll(const common::uuid& ownerDatabaseID) const;
    // Clear any buffer in the WAL writer. Also truncate the WAL file to 0 bytes.
    void clear(BufferManager& bm, bool syncParentDirectory = true);
    bool hasShadowingFH() const { return shadowingFH != nullptr; }
    void setDatabasePath(const std::string& databasePath);
    // Reset the WAL writer to nullptr, and remove the WAL file if it exists.
    void reset();

    // Replay shadow page records from the shadow file to the original data file. This is used
    // during recovery.
    static void replayShadowPageRecords(main::ClientContext& context);
    // Same as above, but for an arbitrary database file (e.g. a partition child's data file)
    // rather than the main database path. `databasePath` is the DATA file path; the shadow
    // path is derived from it.
    static void replayShadowPageRecords(main::ClientContext& context,
        const std::string& databasePath,
        std::optional<common::uuid> legacyDatabaseID = std::nullopt);
    // Variant for files whose handle is already open and locked by the given storage manager
    // (partition children during recovery): avoids taking a second lock on the data file.
    static void replayShadowPageRecordsForStorageManager(main::ClientContext& context,
        StorageManager& storageManager,
        std::optional<common::uuid> legacyDatabaseID = std::nullopt);

private:
    // Taken by PairedReadScope; see it for the contract.
    std::shared_lock<std::shared_mutex> lockForPairedRead() const;

    FileHandle* getOrCreateShadowingFH();
    // Drops the shadow pages created after the savepoint (map entries, records, frames).
    void truncateToSavepoint(ShadowSavepoint savepoint);

    static void replayShadowPageRecordsCore(common::FileInfo& shadowFileInfo,
        common::FileInfo& dataFileInfo, uint64_t maxDatabasePages,
        std::optional<common::uuid> legacyDatabaseID = std::nullopt);

private:
    BufferManager& bm;
    std::string shadowFilePath;
    common::VirtualFileSystem* vfs;
    // This is the file handle for the shadow file. It is created lazily when the first shadow page
    // is created.
    FileHandle* shadowingFH;
    // The map caches shadow page idxes for pages in original files.
    std::unordered_map<common::file_idx_t,
        std::unordered_map<common::page_idx_t, common::page_idx_t>>
        shadowPagesMap;
    std::vector<ShadowPageRecord> shadowPageRecords;
    // Protects shadowPagesMap against concurrent readers (see readShadowVersionIfExists). Only
    // the checkpointing thread modifies the map.
    // Readers hold mtx shared while spinning in optimisticRead on a locked shadow page of a page
    // they reach through ColumnReadWriter::readFromPage (column data pages). The checkpointing
    // thread therefore must not hold such a published shadow page pinned while it takes mtx
    // exclusively (create, publish, clear or reset), or it would deadlock with those readers.
    mutable std::shared_mutex mtx;
    // See lockForPairedRead(). applyMtx is held exclusively while shadow pages are applied and
    // published. std::shared_mutex may prefer readers, so the applying thread also holds
    // applyGateMtx and raises applyPending: new paired readers then wait behind the apply
    // instead of starving it.
    mutable std::mutex applyGateMtx;
    mutable std::shared_mutex applyMtx;
    mutable std::atomic<bool> applyPending{false};
    // Set under applyMtx when applying or publishing fails after the commit record is durable.
    // The data file may then hold a mix of old and new pages, so paired reads fail until the
    // database is reopened and recovery replays the shadow file.
    //
    // Only paired reads need this fence. Primary-key hash indexes are the only structures whose
    // read transactions read the data file directly: their disk arrays and overflow file skip
    // the shadow file outside CHECKPOINT transactions. Column pages are read shadow-first
    // (ColumnReadWriter::readFromPage -> readShadowVersionIfExists), and this failure path
    // keeps the shadow pages published: the WAL is poisoned, and neither postCheckpointCleanup
    // nor rollback runs to clear them. So column scans keep reading complete pages. ART indexes
    // read their disk tree only when disk-backed, and checkpoints do not rewrite those
    // (ArtPrimaryKeyIndex::checkpoint returns early).
    mutable bool applyFailed = false;
    // Savepoint state, owned by the checkpointing thread (see createSavepoint). At most one
    // savepoint is active: node-group checkpoints run one at a time, each taking a savepoint and
    // then releasing it or rolling back to it, which createSavepoint enforces. clear() and
    // reset() also release a savepoint.
    std::optional<ShadowSavepoint> activeSavepoint;
    std::optional<std::string> savepointRollbackFailure;
    std::unordered_map<common::page_idx_t, std::unique_ptr<uint8_t[]>> savedPagesForSavepoint;
    // Fast path for readers: false when there is no shadow page.
    std::atomic<bool> hasShadowPages{false};
};

} // namespace storage
} // namespace lbug

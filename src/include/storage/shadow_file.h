#pragma once

#include <atomic>
#include <functional>
#include <shared_mutex>

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
};
static_assert(std::is_trivially_copyable_v<ShadowFileHeader>);

class BufferManager;
// NOTE: Only the checkpointing thread may create, publish or clear shadow pages. Read
// transactions running concurrently with a checkpoint may only use readShadowVersionIfExists.
class ShadowFile {
public:
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
    // Reads the shadow version of a page if the current checkpoint has one, and returns false
    // without calling readOp otherwise. Unlike the functions above, this is safe to call from
    // read transactions running concurrently with a checkpoint: a checkpoint publishes the new
    // metadata of in-place updated column chunks before the shadow pages are applied to the data
    // file, so those readers must see the shadow version of such pages.
    bool readShadowVersionIfExists(common::file_idx_t originalFile, common::page_idx_t originalPage,
        const std::function<void(uint8_t*)>& readOp) const;

    FileHandle& getShadowingFH() const { return *shadowingFH; }

    void applyShadowPages(StorageManager& storageManager, main::ClientContext& context) const;

    void flushAll(main::ClientContext& context) const;
    // Clear any buffer in the WAL writer. Also truncate the WAL file to 0 bytes.
    void clear(BufferManager& bm);
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
        const std::string& databasePath);
    // Variant for files whose handle is already open and locked by the given storage manager
    // (partition children during recovery): avoids taking a second lock on the data file.
    static void replayShadowPageRecordsForStorageManager(main::ClientContext& context,
        StorageManager& storageManager);

private:
    FileHandle* getOrCreateShadowingFH();

    static void replayShadowPageRecordsCore(common::FileInfo& shadowFileInfo,
        common::FileInfo& dataFileInfo);

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
    // Fast path for readers: false when there is no shadow page.
    std::atomic<bool> hasShadowPages{false};
};

} // namespace storage
} // namespace lbug

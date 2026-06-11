#include "storage/checkpointer.h"

#include <vector>

#include "catalog/catalog.h"
#include "common/file_system/file_system.h"
#include "common/file_system/virtual_file_system.h"
#include "common/serializer/buffered_file.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/in_mem_file_writer.h"
#include "extension/extension_manager.h"
#include "main/client_context.h"
#include "main/database.h"
#include "main/database_manager.h"
#include "main/db_config.h"
#include "storage/buffer_manager/buffer_manager.h"
#include "storage/database_header.h"
#include "storage/shadow_utils.h"
#include "storage/storage_manager.h"
#include "storage/storage_version_info.h"
#include "storage/wal/local_wal.h"
#include "transaction/transaction.h"

namespace lbug {
namespace storage {

namespace {

void writeDatabaseHeaderToStorage(main::ClientContext& clientContext, const DatabaseHeader& header,
    StorageManager& storageManager) {
    auto headerWriter =
        std::make_shared<common::InMemFileWriter>(*MemoryManager::Get(clientContext));
    common::Serializer headerSerializer(headerWriter);
    header.serialize(headerSerializer);
    auto headerPage = headerWriter->getPage(0);

    auto dataFH = storageManager.getDataFH();
    auto& shadowFile = storageManager.getShadowFile();
    auto shadowHeader = ShadowUtils::createShadowVersionIfNecessaryAndPinPage(
        common::StorageConstants::DB_HEADER_PAGE_IDX, true /* skipReadingOriginalPage */, *dataFH,
        shadowFile);
    memcpy(shadowHeader.frame, headerPage.data(), common::LBUG_PAGE_SIZE);
    shadowFile.getShadowingFH().unpinPage(shadowHeader.shadowPage);

    storageManager.setDatabaseHeader(std::make_unique<DatabaseHeader>(header));
}

void logCheckpointAndApplyShadowPagesForStorage(main::ClientContext& clientContext,
    StorageManager& storageManager, bool walRotated) {
    auto& shadowFile = storageManager.getShadowFile();
    shadowFile.flushAll(clientContext);
    auto wal = &storageManager.getWAL();
    if (walRotated) {
        wal->logAndFlushCheckpointToFrozen(&clientContext);
    } else {
        wal->logAndFlushCheckpoint(&clientContext);
    }
    shadowFile.applyShadowPages(storageManager, clientContext);
    auto bufferManager = MemoryManager::Get(clientContext)->getBufferManager();
    if (!walRotated) {
        wal->clear();
    }
    shadowFile.clear(*bufferManager);
}

} // namespace

Checkpointer::Checkpointer(main::ClientContext& clientContext)
    : clientContext{clientContext},
      isInMemory{main::DBConfig::isDBPathInMemory(clientContext.getDatabasePath())},
      mainStorageManager{clientContext.getDatabase()->getStorageManager()} {}

Checkpointer::~Checkpointer() = default;

std::vector<Checkpointer::CheckpointTarget> Checkpointer::collectCheckpointTargets() const {
    std::vector<CheckpointTarget> result;
    result.push_back({clientContext.getDatabase()->getCatalog(), mainStorageManager});
    for (auto* graphCatalog : clientContext.getDatabase()->getDatabaseManager()->getGraphs()) {
        if (auto* graphStorageManager = graphCatalog->getStorageManager()) {
            result.push_back({graphCatalog, graphStorageManager});
        }
    }
    return result;
}

PageRange Checkpointer::serializeCatalog(const catalog::Catalog& catalog,
    StorageManager& storageManager) {
    auto catalogWriter =
        std::make_shared<common::InMemFileWriter>(*MemoryManager::Get(clientContext));
    common::Serializer catalogSerializer(catalogWriter);
    catalog.serialize(catalogSerializer);
    auto pageAllocator = storageManager.getDataFH()->getPageManager();
    return catalogWriter->flush(*pageAllocator, storageManager.getShadowFile());
}

PageRange Checkpointer::serializeCatalogSnapshot(const catalog::Catalog& catalog,
    StorageManager& storageManager) {
    auto catalogWriter =
        std::make_shared<common::InMemFileWriter>(*MemoryManager::Get(clientContext));
    common::Serializer catalogSerializer(catalogWriter);
    catalog.serializeSnapshot(catalogSerializer, snapshotTS);
    auto pageAllocator = storageManager.getDataFH()->getPageManager();
    return catalogWriter->flush(*pageAllocator, storageManager.getShadowFile());
}

PageRange Checkpointer::serializeMetadataSnapshot(const catalog::Catalog& catalog,
    StorageManager& storageManager) {
    auto metadataWriter =
        std::make_shared<common::InMemFileWriter>(*MemoryManager::Get(clientContext));
    common::Serializer metadataSerializer(metadataWriter);
    const transaction::Transaction snapshotTxn(transaction::TransactionType::CHECKPOINT,
        transaction::Transaction::DUMMY_TRANSACTION_ID, snapshotTS);
    storageManager.serialize(catalog, snapshotTxn, metadataSerializer);

    auto& pageManager = *storageManager.getDataFH()->getPageManager();
    const auto pagesForPageManager = pageManager.estimatePagesNeededForSerialize();
    auto pageAllocator = storageManager.getDataFH()->getPageManager();
    const auto allocatedPages = pageAllocator->allocatePageRange(
        metadataWriter->getNumPagesToFlush() + pagesForPageManager);
    pageManager.serialize(metadataSerializer);

    metadataWriter->flush(allocatedPages, pageAllocator->getDataFH(),
        storageManager.getShadowFile());
    return allocatedPages;
}

PageRange Checkpointer::serializeMetadata(const catalog::Catalog& catalog,
    StorageManager& storageManager) {
    auto metadataWriter =
        std::make_shared<common::InMemFileWriter>(*MemoryManager::Get(clientContext));
    common::Serializer metadataSerializer(metadataWriter);
    storageManager.serialize(catalog, metadataSerializer);

    // We need to preallocate the pages for the page manager before we actually serialize it,
    // this is because the page manager needs to track the pages used for itself.
    // The number of pages needed for the page manager should only decrease after making an
    // additional allocation, so we just calculate the number of pages needed to serialize the
    // current state of the page manager.
    // Thus, it is possible that we allocate an extra page that we won't end up writing to when we
    // flush the metadata writer. This may cause a discrepancy between the number of tracked pages
    // and the number of physical pages in the file but shouldn't cause any actual incorrect
    // behavior in the database.
    auto& pageManager = *storageManager.getDataFH()->getPageManager();
    const auto pagesForPageManager = pageManager.estimatePagesNeededForSerialize();
    auto pageAllocator = storageManager.getDataFH()->getPageManager();
    const auto allocatedPages = pageAllocator->allocatePageRange(
        metadataWriter->getNumPagesToFlush() + pagesForPageManager);
    pageManager.serialize(metadataSerializer);

    metadataWriter->flush(allocatedPages, pageAllocator->getDataFH(),
        storageManager.getShadowFile());
    return allocatedPages;
}

void Checkpointer::writeCheckpoint() {
    if (isInMemory) {
        return;
    }

    checkpointTargets = collectCheckpointTargets();

    for (const auto& target : checkpointTargets) {
        auto rotated = target.storageManager->getWAL().rotateForCheckpoint(&clientContext);
        walRotatedByManager[target.storageManager] = rotated;
        walRotated = walRotated || rotated;
    }

    auto databaseHeader = *mainStorageManager->getOrInitDatabaseHeader(clientContext);
    const auto oldStorageVersion = databaseHeader.storageVersion;
    databaseHeader.storageVersion = StorageVersionInfo::getStorageVersion();
    hasStorageVersionUpgrade = oldStorageVersion != databaseHeader.storageVersion;
    bool localHasStorageChanges = checkpointStorage();
    serializeCatalogAndMetadata(databaseHeader, localHasStorageChanges);
    databaseHeader.dataFileNumPages = mainStorageManager->getDataFH()->getNumPages();
    writeDatabaseHeader(databaseHeader);
    logCheckpointAndApplyShadowPages(walRotatedByManager.at(mainStorageManager));
    for (const auto& target : checkpointTargets) {
        if (target.storageManager == mainStorageManager) {
            continue;
        }
        logCheckpointAndApplyShadowPagesForStorage(clientContext, *target.storageManager,
            walRotatedByManager.at(target.storageManager));
    }

    // Snapshot versions while the write gate is still held.
    for (const auto& target : checkpointTargets) {
        catalogVersionAtCheckpointByCatalog[target.catalog] = target.catalog->getVersion();
        pageManagerVersionAtCheckpointByManager[target.storageManager] =
            target.storageManager->getDataFH()->getPageManager()->getVersion();
    }
    catalogVersionAtCheckpoint =
        catalogVersionAtCheckpointByCatalog[clientContext.getDatabase()->getCatalog()];
    pageManagerVersionAtCheckpoint = pageManagerVersionAtCheckpointByManager[mainStorageManager];

    postCheckpointCleanup();
}

void Checkpointer::beginCheckpoint(common::transaction_t snapshotTimestamp) {
    if (isInMemory) {
        return;
    }

    snapshotTS = snapshotTimestamp;
    checkpointTargets = collectCheckpointTargets();

    for (const auto& target : checkpointTargets) {
        auto rotated = target.storageManager->getWAL().rotateForCheckpoint(&clientContext);
        walRotatedByManager[target.storageManager] = rotated;
        walRotated = walRotated || rotated;
    }

    checkpointHeader = *mainStorageManager->getOrInitDatabaseHeader(clientContext);
    const auto oldStorageVersion = checkpointHeader.storageVersion;
    checkpointHeader.storageVersion = StorageVersionInfo::getStorageVersion();
    hasStorageVersionUpgrade = oldStorageVersion != checkpointHeader.storageVersion;

    // Capture versions while the write gate is still held.
    for (const auto& target : checkpointTargets) {
        catalogVersionAtCheckpointByCatalog[target.catalog] = target.catalog->getVersion();
        pageManagerVersionAtCheckpointByManager[target.storageManager] =
            target.storageManager->getDataFH()->getPageManager()->getVersion();
        tableEpochWatermarksByManager[target.storageManager] =
            target.storageManager->captureChangeEpochs();
    }
    catalogVersionAtCheckpoint =
        catalogVersionAtCheckpointByCatalog[clientContext.getDatabase()->getCatalog()];
    pageManagerVersionAtCheckpoint = pageManagerVersionAtCheckpointByManager[mainStorageManager];
    tableEpochWatermarks = tableEpochWatermarksByManager[mainStorageManager];
}

void Checkpointer::checkpointStoragePhase() {
    if (isInMemory) {
        return;
    }
    hasStorageChanges = checkpointStorage();
}

void Checkpointer::finishCheckpoint() {
    if (isInMemory) {
        return;
    }
    // NOTE: finishCheckpoint() runs after the write gate has been released (when WAL rotation
    // occurred).  New DDL/write transactions may therefore be active, but they assign timestamps
    // strictly greater than the snapshotTS captured under the gate in beginCheckpoint().
    // serializeCatalogAndMetadata() uses snapshotTS > 0 to choose serializeCatalogSnapshot(),
    // which serializes only catalog entries whose commit timestamp is <= snapshotTS, so no
    // post-gate DDL mutation is visible in the serialized snapshot.
    serializeCatalogAndMetadata(checkpointHeader, hasStorageChanges);
    checkpointHeader.dataFileNumPages = mainStorageManager->getDataFH()->getNumPages();
    writeDatabaseHeader(checkpointHeader);
    logCheckpointAndApplyShadowPages(walRotatedByManager.at(mainStorageManager));
    for (const auto& target : checkpointTargets) {
        if (target.storageManager == mainStorageManager) {
            continue;
        }
        logCheckpointAndApplyShadowPagesForStorage(clientContext, *target.storageManager,
            walRotatedByManager.at(target.storageManager));
    }
}

void Checkpointer::postCheckpointCleanup(bool canResetPageManagerToCurrent) {
    if (isInMemory) {
        return;
    }
    // NOTE: No try/catch here is intentional. By the time this runs, finishCheckpoint() has
    // already persisted the checkpoint header and applied shadow pages — the database is
    // durable.  Any exception in the in-memory cleanup below indicates a programming error;
    // letting it propagate (and crash the process) is safer than continuing with partially
    // reset in-memory state.  On the next startup the database loads from the stable
    // on-disk checkpoint and is fully consistent.
    mainStorageManager->finalizeCheckpoint();
    for (const auto& target : checkpointTargets) {
        if (target.storageManager == mainStorageManager) {
            continue;
        }
        target.storageManager->finalizeCheckpoint();
    }
    auto bufferManager = MemoryManager::Get(clientContext)->getBufferManager();
    bufferManager->removeEvictedCandidates();

    for (const auto& target : checkpointTargets) {
        if (catalogVersionAtCheckpointByCatalog.contains(target.catalog)) {
            target.catalog->resetVersion(catalogVersionAtCheckpointByCatalog[target.catalog]);
        }
        const auto walRotated = walRotatedByManager.at(target.storageManager);
        const auto hasPostCheckpointWAL =
            walRotated && target.storageManager->getWAL().getFileSize() > 0;
        if (pageManagerVersionAtCheckpointByManager.contains(target.storageManager)) {
            auto* pageManager = target.storageManager->getDataFH()->getPageManager();
            if (hasPostCheckpointWAL || !canResetPageManagerToCurrent) {
                pageManager->resetVersion(
                    pageManagerVersionAtCheckpointByManager[target.storageManager]);
            } else {
                pageManager->resetVersion();
            }
        }
        if (walRotated) {
            target.storageManager->getWAL().clearFrozenWAL();
        } else {
            target.storageManager->getWAL().reset();
        }
        target.storageManager->getShadowFile().reset();
    }
}

bool Checkpointer::checkpointStorage() {
    bool hasChanges = false;
    for (const auto& target : checkpointTargets) {
        auto pageAllocator = target.storageManager->getDataFH()->getPageManager();
        bool targetHasChanges;
        if (snapshotTS > 0) {
            const transaction::Transaction snapshotTxn(transaction::TransactionType::CHECKPOINT,
                transaction::Transaction::DUMMY_TRANSACTION_ID, snapshotTS);
            targetHasChanges =
                target.storageManager->checkpoint(&clientContext, *target.catalog, snapshotTxn,
                    *pageAllocator, tableEpochWatermarksByManager.at(target.storageManager));
        } else {
            targetHasChanges =
                target.storageManager->checkpoint(&clientContext, *target.catalog, *pageAllocator);
        }
        storageChangesByManager[target.storageManager] = targetHasChanges;
        hasChanges = targetHasChanges || hasChanges;
    }
    return hasChanges;
}

void Checkpointer::serializeCatalogAndMetadata(DatabaseHeader& databaseHeader,
    bool storageChanges) {
    // IMPORTANT: Always use the main database's catalog, not Catalog::Get()
    // which might return a graph's catalog if a default graph is set!
    const auto catalog = clientContext.getDatabase()->getCatalog();
    auto* dataFH = mainStorageManager->getDataFH();
    const bool useSnapshot = snapshotTS > 0;

    if (databaseHeader.catalogPageRange.startPageIdx == common::INVALID_PAGE_IDX ||
        catalog->changedSinceLastCheckpoint() || hasStorageVersionUpgrade) {
        databaseHeader.updateCatalogPageRange(*dataFH->getPageManager(),
            useSnapshot ? serializeCatalogSnapshot(*catalog, *mainStorageManager) :
                          serializeCatalog(*catalog, *mainStorageManager));
    }
    if (databaseHeader.metadataPageRange.startPageIdx == common::INVALID_PAGE_IDX ||
        storageChanges || catalog->changedSinceLastCheckpoint() ||
        dataFH->getPageManager()->changedSinceLastCheckpoint()) {
        databaseHeader.freeMetadataPageRange(*dataFH->getPageManager());
        databaseHeader.metadataPageRange =
            useSnapshot ? serializeMetadataSnapshot(*catalog, *mainStorageManager) :
                          serializeMetadata(*catalog, *mainStorageManager);
    }

    for (const auto& target : checkpointTargets) {
        if (target.storageManager == mainStorageManager) {
            continue;
        }
        auto graphHeader = *target.storageManager->getOrInitDatabaseHeader(clientContext);
        auto* graphDataFH = target.storageManager->getDataFH();
        const auto graphStorageChanges = storageChangesByManager.at(target.storageManager);
        if (graphHeader.catalogPageRange.startPageIdx == common::INVALID_PAGE_IDX ||
            target.catalog->changedSinceLastCheckpoint() || hasStorageVersionUpgrade) {
            graphHeader.updateCatalogPageRange(*graphDataFH->getPageManager(),
                useSnapshot ? serializeCatalogSnapshot(*target.catalog, *target.storageManager) :
                              serializeCatalog(*target.catalog, *target.storageManager));
        }
        if (graphHeader.metadataPageRange.startPageIdx == common::INVALID_PAGE_IDX ||
            graphStorageChanges || target.catalog->changedSinceLastCheckpoint() ||
            graphDataFH->getPageManager()->changedSinceLastCheckpoint()) {
            graphHeader.freeMetadataPageRange(*graphDataFH->getPageManager());
            graphHeader.metadataPageRange =
                useSnapshot ? serializeMetadataSnapshot(*target.catalog, *target.storageManager) :
                              serializeMetadata(*target.catalog, *target.storageManager);
        }
        graphHeader.dataFileNumPages = graphDataFH->getNumPages();
        writeDatabaseHeaderToStorage(clientContext, graphHeader, *target.storageManager);
    }
}

void Checkpointer::writeDatabaseHeader(const DatabaseHeader& header) {
    writeDatabaseHeaderToStorage(clientContext, header, *mainStorageManager);
}

void Checkpointer::logCheckpointAndApplyShadowPages(bool walRotated_) {
    logCheckpointAndApplyShadowPagesForStorage(clientContext, *mainStorageManager, walRotated_);
}

void Checkpointer::rollback() {
    if (isInMemory) {
        return;
    }
    // Any pages freed during the checkpoint are no longer freed
    for (const auto& target : checkpointTargets) {
        target.storageManager->rollbackCheckpoint(*target.catalog);
    }
}

bool Checkpointer::canAutoCheckpoint(const main::ClientContext& clientContext,
    const transaction::Transaction& transaction) {
    if (clientContext.isInMemory()) {
        return false;
    }
    if (!clientContext.getDBConfig()->autoCheckpoint) {
        return false;
    }
    if (transaction.isRecovery()) {
        // Recovery transactions are not allowed to trigger auto checkpoint.
        return false;
    }
    auto wal = &clientContext.getDatabase()->getStorageManager()->getWAL();
    const auto expectedSize = transaction.getLocalWAL().getSize() + wal->getFileSize();
    return expectedSize > clientContext.getDBConfig()->checkpointThreshold;
}

void Checkpointer::readCheckpoint() {
    // IMPORTANT: Use the main database's storage manager, NOT StorageManager::Get() which
    // returns the graph's storage manager if a default graph exists!
    auto storageManager = clientContext.getDatabase()->getStorageManager();
    storageManager->initDataFileHandle(common::VirtualFileSystem::GetUnsafe(clientContext),
        &clientContext);
    if (!isInMemory && storageManager->getDataFH()->getNumPages() > 0) {
        readCheckpoint(&clientContext, clientContext.getDatabase()->getCatalog(), storageManager);
    }
    extension::ExtensionManager::Get(clientContext)->autoLoadLinkedExtensions(&clientContext);
}

void Checkpointer::readCheckpoint(main::ClientContext* context, catalog::Catalog* catalog,
    StorageManager* storageManager) {
    auto fileInfo = storageManager->getDataFH()->getFileInfo();
    auto reader = std::make_unique<common::BufferedFileReader>(*fileInfo);
    common::Deserializer deSer(std::move(reader));
    auto currentHeader = std::make_unique<DatabaseHeader>(DatabaseHeader::deserialize(deSer));
    // If the catalog page range is invalid, it means there is no catalog to read; thus, the
    // database is empty.
    if (currentHeader->catalogPageRange.startPageIdx != common::INVALID_PAGE_IDX) {
        deSer.getReader()->cast<common::BufferedFileReader>()->resetReadOffset(
            currentHeader->catalogPageRange.startPageIdx * common::LBUG_PAGE_SIZE);
        catalog->deserialize(deSer);
        deSer.getReader()->cast<common::BufferedFileReader>()->resetReadOffset(
            currentHeader->metadataPageRange.startPageIdx * common::LBUG_PAGE_SIZE);
        storageManager->deserialize(context, catalog, deSer);
        storageManager->getDataFH()->getPageManager()->deserialize(deSer);
        storageManager->getDataFH()->getPageManager()->reclaimTailPagesIfNeeded(
            currentHeader->dataFileNumPages);
    }
    storageManager->setDatabaseHeader(std::move(currentHeader));
}

} // namespace storage
} // namespace lbug

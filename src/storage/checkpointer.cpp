#include "storage/checkpointer.h"

#include "catalog/catalog.h"
#include "common/file_system/file_system.h"
#include "common/file_system/virtual_file_system.h"
#include "common/serializer/buffered_file.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/in_mem_file_writer.h"
#include "extension/extension_manager.h"
#include "main/client_context.h"
#include "main/db_config.h"
#include "storage/buffer_manager/buffer_manager.h"
#include "storage/database_header.h"
#include "storage/shadow_utils.h"
#include "storage/storage_manager.h"
#include "storage/wal/local_wal.h"
#include "transaction/transaction.h"

namespace lbug {
namespace storage {

Checkpointer::Checkpointer(main::ClientContext& clientContext)
    : clientContext{clientContext},
      isInMemory{main::DBConfig::isDBPathInMemory(clientContext.getDatabasePath())},
      mainStorageManager{clientContext.getDatabase()->getStorageManager()} {}

Checkpointer::~Checkpointer() = default;

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

    walRotated = mainStorageManager->getWAL().rotateForCheckpoint(&clientContext);

    auto databaseHeader = *mainStorageManager->getOrInitDatabaseHeader(clientContext);
    bool localHasStorageChanges = checkpointStorage();
    serializeCatalogAndMetadata(databaseHeader, localHasStorageChanges);
    databaseHeader.dataFileNumPages = mainStorageManager->getDataFH()->getNumPages();
    writeDatabaseHeader(databaseHeader);
    logCheckpointAndApplyShadowPages(walRotated);

    // Snapshot versions while the write gate is still held.
    catalogVersionAtCheckpoint = clientContext.getDatabase()->getCatalog()->getVersion();
    pageManagerVersionAtCheckpoint =
        mainStorageManager->getDataFH()->getPageManager()->getVersion();

    postCheckpointCleanup();
}

void Checkpointer::beginCheckpoint(common::transaction_t snapshotTimestamp) {
    if (isInMemory) {
        return;
    }

    snapshotTS = snapshotTimestamp;

    walRotated = mainStorageManager->getWAL().rotateForCheckpoint(&clientContext);

    checkpointHeader = *mainStorageManager->getOrInitDatabaseHeader(clientContext);

    // Capture versions while the write gate is still held.
    catalogVersionAtCheckpoint = clientContext.getDatabase()->getCatalog()->getVersion();
    pageManagerVersionAtCheckpoint =
        mainStorageManager->getDataFH()->getPageManager()->getVersion();
    tableEpochWatermarks = mainStorageManager->captureChangeEpochs();
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
    serializeCatalogAndMetadata(checkpointHeader, hasStorageChanges);
    checkpointHeader.dataFileNumPages = mainStorageManager->getDataFH()->getNumPages();
    writeDatabaseHeader(checkpointHeader);
    logCheckpointAndApplyShadowPages(walRotated);
}

void Checkpointer::postCheckpointCleanup() {
    if (isInMemory) {
        return;
    }

    mainStorageManager->finalizeCheckpoint();
    auto bufferManager = MemoryManager::Get(clientContext)->getBufferManager();
    bufferManager->removeEvictedCandidates();

    clientContext.getDatabase()->getCatalog()->resetVersion(catalogVersionAtCheckpoint);
    auto* dataFH = mainStorageManager->getDataFH();
    dataFH->getPageManager()->resetVersion(pageManagerVersionAtCheckpoint);
    if (walRotated) {
        mainStorageManager->getWAL().clearFrozenWAL();
    } else {
        mainStorageManager->getWAL().reset();
    }
    mainStorageManager->getShadowFile().reset();
}

bool Checkpointer::checkpointStorage() {
    auto pageAllocator = mainStorageManager->getDataFH()->getPageManager();
    if (snapshotTS > 0) {
        const transaction::Transaction snapshotTxn(transaction::TransactionType::CHECKPOINT,
            transaction::Transaction::DUMMY_TRANSACTION_ID, snapshotTS);
        return mainStorageManager->checkpoint(&clientContext, snapshotTxn, *pageAllocator,
            tableEpochWatermarks);
    }
    return mainStorageManager->checkpoint(&clientContext, *pageAllocator);
}

void Checkpointer::serializeCatalogAndMetadata(DatabaseHeader& databaseHeader,
    bool storageChanges) {
    // IMPORTANT: Always use the main database's catalog, not Catalog::Get()
    // which might return a graph's catalog if a default graph is set!
    const auto catalog = clientContext.getDatabase()->getCatalog();
    auto* dataFH = mainStorageManager->getDataFH();
    const bool useSnapshot = snapshotTS > 0;

    if (databaseHeader.catalogPageRange.startPageIdx == common::INVALID_PAGE_IDX ||
        catalog->changedSinceLastCheckpoint()) {
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
}

void Checkpointer::writeDatabaseHeader(const DatabaseHeader& header) {
    auto headerWriter =
        std::make_shared<common::InMemFileWriter>(*MemoryManager::Get(clientContext));
    common::Serializer headerSerializer(headerWriter);
    header.serialize(headerSerializer);
    auto headerPage = headerWriter->getPage(0);

    auto dataFH = mainStorageManager->getDataFH();
    auto& shadowFile = mainStorageManager->getShadowFile();
    auto shadowHeader = ShadowUtils::createShadowVersionIfNecessaryAndPinPage(
        common::StorageConstants::DB_HEADER_PAGE_IDX, true /* skipReadingOriginalPage */, *dataFH,
        shadowFile);
    memcpy(shadowHeader.frame, headerPage.data(), common::LBUG_PAGE_SIZE);
    shadowFile.getShadowingFH().unpinPage(shadowHeader.shadowPage);

    // Update the in-memory database header with the new version
    mainStorageManager->setDatabaseHeader(std::make_unique<DatabaseHeader>(header));
}

void Checkpointer::logCheckpointAndApplyShadowPages(bool walRotated_) {
    auto& shadowFile = mainStorageManager->getShadowFile();
    shadowFile.flushAll(clientContext);
    auto wal = WAL::Get(clientContext);
    if (walRotated_) {
        wal->logAndFlushCheckpointToFrozen(&clientContext);
    } else {
        wal->logAndFlushCheckpoint(&clientContext);
    }
    shadowFile.applyShadowPages(*mainStorageManager, clientContext);
    auto bufferManager = MemoryManager::Get(clientContext)->getBufferManager();
    if (!walRotated_) {
        wal->clear();
    }
    shadowFile.clear(*bufferManager);
}

void Checkpointer::rollback() {
    if (isInMemory) {
        return;
    }
    auto catalog = catalog::Catalog::Get(clientContext);
    // Any pages freed during the checkpoint are no longer freed
    mainStorageManager->rollbackCheckpoint(*catalog);
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
    auto wal = WAL::Get(clientContext);
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
        readCheckpoint(&clientContext, catalog::Catalog::Get(clientContext), storageManager);
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

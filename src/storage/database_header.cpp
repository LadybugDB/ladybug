#include "storage/database_header.h"

#include <cstring>

#include "common/exception/runtime.h"
#include "common/file_system/file_info.h"
#include "common/serializer/buffered_file.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "common/system_config.h"
#include "main/client_context.h"
#include "storage/page_manager.h"
#include "storage/storage_version_info.h"
#include <format>

namespace lbug::storage {
static void validateStorageVersion(common::Deserializer& deSer) {
    std::string key;
    deSer.validateDebuggingInfo(key, "storage_version");
    storage_version_t savedStorageVersion = 0;
    deSer.deserializeValue(savedStorageVersion);
    const auto storageVersion = StorageVersionInfo::getStorageVersion();
    if (savedStorageVersion != storageVersion) {
        // TODO(Guodong): Add a test case for this.
        throw common::RuntimeException(
            std::format("Trying to read a database file with a different version. "
                        "Database file version: {}, Current build storage version: {}",
                savedStorageVersion, storageVersion));
    }
}

static void validateMagicBytes(common::Deserializer& deSer) {
    std::string key;
    deSer.validateDebuggingInfo(key, "magic");
    const auto numMagicBytes = strlen(StorageVersionInfo::MAGIC_BYTES);
    uint8_t magicBytes[4];
    for (auto i = 0u; i < numMagicBytes; i++) {
        deSer.deserializeValue<uint8_t>(magicBytes[i]);
    }
    if (memcmp(magicBytes, StorageVersionInfo::MAGIC_BYTES, numMagicBytes) != 0) {
        throw common::RuntimeException(
            "Unable to open database. The file is not a valid Lbug database file!");
    }
}

void DatabaseHeader::updateCatalogPageRange(PageManager& pageManager, PageRange newPageRange) {
    if (catalogPageRange.startPageIdx != common::INVALID_PAGE_IDX) {
        pageManager.freePageRange(catalogPageRange);
    }
    catalogPageRange = newPageRange;
}

void DatabaseHeader::freeMetadataPageRange(PageManager& pageManager) const {
    if (metadataPageRange.startPageIdx != common::INVALID_PAGE_IDX) {
        pageManager.freePageRange(metadataPageRange);
    }
}

static constexpr uint8_t HEADER_FORMAT_VERSION_WITH_DATAFILE_NUM_PAGES = 2;

static void writeMagicBytes(common::Serializer& serializer) {
    serializer.writeDebuggingInfo("magic");
    const auto numMagicBytes = strlen(StorageVersionInfo::MAGIC_BYTES);
    for (auto i = 0u; i < numMagicBytes; i++) {
        serializer.serializeValue<uint8_t>(StorageVersionInfo::MAGIC_BYTES[i]);
    }
}

void DatabaseHeader::serialize(common::Serializer& ser) const {
    writeMagicBytes(ser);
    ser.writeDebuggingInfo("storage_version");
    ser.serializeValue(StorageVersionInfo::getStorageVersion());
    ser.writeDebuggingInfo("catalog");
    ser.serializeValue(catalogPageRange.startPageIdx);
    ser.serializeValue(catalogPageRange.numPages);
    ser.writeDebuggingInfo("metadata");
    ser.serializeValue(metadataPageRange.startPageIdx);
    ser.serializeValue(metadataPageRange.numPages);
    ser.writeDebuggingInfo("databaseID");
    ser.serializeValue(databaseID.value);
    // No debugging info so old readers can stop here; new readers read 9 bytes (version + size).
    ser.serializeValue(static_cast<uint8_t>(HEADER_FORMAT_VERSION_WITH_DATAFILE_NUM_PAGES));
    ser.serializeValue(dataFileNumPages);
}

DatabaseHeader DatabaseHeader::deserialize(common::Deserializer& deSer) {
    validateMagicBytes(deSer);
    validateStorageVersion(deSer);
    PageRange catalogPageRange{}, metaPageRange{};
    common::ku_uuid_t databaseID{};
    std::string key;
    deSer.validateDebuggingInfo(key, "catalog");
    deSer.deserializeValue(catalogPageRange.startPageIdx);
    deSer.deserializeValue(catalogPageRange.numPages);
    deSer.validateDebuggingInfo(key, "metadata");
    deSer.deserializeValue(metaPageRange.startPageIdx);
    deSer.deserializeValue(metaPageRange.numPages);
    deSer.validateDebuggingInfo(key, "databaseID");
    deSer.deserializeValue(databaseID.value);
    common::page_idx_t dataFileNumPages = 0;
    uint8_t headerFormatVersion = 0;
    // Old format (e.g. lbug-0.14.1) ends here; reading 9 bytes would read beyond logical header.
    // Read one byte: only if it matches our format version do we read the following 8 bytes.
    deSer.deserializeValue(headerFormatVersion);
    if (headerFormatVersion == HEADER_FORMAT_VERSION_WITH_DATAFILE_NUM_PAGES) {
        deSer.deserializeValue(dataFileNumPages);
    }
    return {catalogPageRange, metaPageRange, dataFileNumPages, databaseID};
}

DatabaseHeader DatabaseHeader::createInitialHeader(common::RandomEngine* randomEngine) {
    // We generate a random UUID to act as the database ID
    return DatabaseHeader{{}, {}, 0, common::UUID::generateRandomUUID(randomEngine)};
}

std::optional<DatabaseHeader> DatabaseHeader::readDatabaseHeader(common::FileInfo& dataFileInfo) {
    if (dataFileInfo.getFileSize() < common::LBUG_PAGE_SIZE) {
        // If the data file hasn't been written to there is no existing database header
        return std::nullopt;
    }
    auto reader = std::make_unique<common::BufferedFileReader>(dataFileInfo);
    common::Deserializer deSer(std::move(reader));
    try {
        return DatabaseHeader::deserialize(deSer);
    } catch (const common::RuntimeException&) {
        // It is possible we optimistically write to the database file before the first checkpoint
        // In this case the magic bytes check will fail and we assume there is no existing header
        return std::nullopt;
    }
}
} // namespace lbug::storage

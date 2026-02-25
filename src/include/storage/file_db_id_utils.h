#pragma once

#include "common/file_system/file_info.h"
#include "common/serializer/serializer.h"
#include "common/types/uuid.h"
namespace lbug {
namespace storage {
struct FileDBIDUtils {
    // For some temporary DB files such as the WAL and shadow file
    // We want to verify that they actually match the current database before replaying
    // We do this by adding a unique UUID to the header of the data.lbdb file
    // And making sure they match the IDs of the temporary files
    static void verifyDatabaseID(const common::FileInfo& fileInfo, common::uuid expectedDatabaseID,
        common::uuid databaseID);
    static void writeDatabaseID(common::Serializer& ser, common::uuid databaseID);
};
} // namespace storage
} // namespace lbug

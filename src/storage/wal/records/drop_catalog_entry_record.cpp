#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "storage/wal/wal_record.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

void DropCatalogEntryRecord::serialize(Serializer& serializer) const {
    WALRecord::serialize(serializer);
    serializer.write<oid_t>(entryID);
    serializer.write<catalog::CatalogEntryType>(entryType);
}

std::unique_ptr<DropCatalogEntryRecord> DropCatalogEntryRecord::deserialize(
    Deserializer& deserializer) {
    auto retVal = std::make_unique<DropCatalogEntryRecord>();
    deserializer.deserializeValue(retVal->entryID);
    deserializer.deserializeValue(retVal->entryType);
    return retVal;
}

} // namespace storage
} // namespace lbug

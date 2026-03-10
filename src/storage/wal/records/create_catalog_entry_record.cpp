#include "catalog/catalog_entry/catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "storage/wal/wal_record.h"

using namespace lbug::common;
using namespace lbug::catalog;

namespace lbug {
namespace storage {

void CreateCatalogEntryRecord::serialize(Serializer& serializer) const {
    WALRecord::serialize(serializer);
    catalogEntry->serialize(serializer);
    serializer.serializeValue(isInternal);
}

std::unique_ptr<CreateCatalogEntryRecord> CreateCatalogEntryRecord::deserialize(
    Deserializer& deserializer) {
    auto retVal = std::make_unique<CreateCatalogEntryRecord>();
    retVal->ownedCatalogEntry = CatalogEntry::deserialize(deserializer);
    bool isInternal = false;
    deserializer.deserializeValue(isInternal);
    retVal->isInternal = isInternal;
    return retVal;
}

} // namespace storage
} // namespace lbug

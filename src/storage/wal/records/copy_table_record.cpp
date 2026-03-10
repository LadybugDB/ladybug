#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "storage/wal/wal_record.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

void CopyTableRecord::serialize(Serializer& serializer) const {
    WALRecord::serialize(serializer);
    serializer.write(tableID);
}

std::unique_ptr<CopyTableRecord> CopyTableRecord::deserialize(Deserializer& deserializer) {
    auto retVal = std::make_unique<CopyTableRecord>();
    deserializer.deserializeValue(retVal->tableID);
    return retVal;
}

} // namespace storage
} // namespace lbug

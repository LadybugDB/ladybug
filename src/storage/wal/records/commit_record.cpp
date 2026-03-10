#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "storage/wal/wal_record.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

void CommitRecord::serialize(Serializer& serializer) const {
    WALRecord::serialize(serializer);
}

std::unique_ptr<CommitRecord> CommitRecord::deserialize(Deserializer&) {
    return std::make_unique<CommitRecord>();
}

} // namespace storage
} // namespace lbug

#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "storage/wal/wal_record.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

void CheckpointRecord::serialize(Serializer& serializer) const {
    WALRecord::serialize(serializer);
}

std::unique_ptr<CheckpointRecord> CheckpointRecord::deserialize(Deserializer&) {
    return std::make_unique<CheckpointRecord>();
}

} // namespace storage
} // namespace lbug

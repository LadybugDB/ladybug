#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "storage/wal/wal_record.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

void BeginTransactionRecord::serialize(Serializer& serializer) const {
    WALRecord::serialize(serializer);
}

std::unique_ptr<BeginTransactionRecord> BeginTransactionRecord::deserialize(Deserializer&) {
    return std::make_unique<BeginTransactionRecord>();
}

} // namespace storage
} // namespace lbug
